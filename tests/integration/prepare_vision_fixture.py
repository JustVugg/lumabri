"""Combine independent tiny language/vision models for resident API tests.

This fixture checks image arithmetic and transport, not visual answer quality.
The vision oracle comes from Transformers with the upstream test weights.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--engine", required=True, type=Path)
parser.add_argument("--text", required=True, type=Path)
parser.add_argument("--out", required=True, type=Path)
args = parser.parse_args()
assert not args.out.exists(), "use a new fixture destination"
import numpy as np
from PIL import Image
import torch
from safetensors.torch import load_file, save_file
from transformers.models.qwen4_exp.configuration_qwen4_exp import Qwen4ExpVisionConfig
from transformers.models.qwen4_exp.modeling_qwen4_exp import Qwen4ExpVisionModel
torch.set_num_threads(2)
with tempfile.TemporaryDirectory(prefix="lmb-vision-reference-") as tmp:
    subprocess.run([sys.executable, str(args.engine / "tools/make_qwen38_vision_tiny.py"), "--out", tmp], check=True)
    visual_config = json.loads((Path(tmp) / "config.json").read_text())["vision_config"]
    vision_weights = load_file(Path(tmp) / "model.safetensors")
    text_config = json.loads((args.text / "config.json").read_text())
    assert text_config["hidden_size"] == visual_config["out_hidden_size"] == 32
    assert text_config["vocab_size"] == 256
    text_config["vocab_size"] = 260
    weights = load_file(args.text / "model.safetensors")
    for key in ("model.embed_tokens.weight", "lm_head.weight"):
        weights[key] = torch.cat([weights[key], weights[key][:4].clone()], dim=0)
    # A byte-level synthetic model can otherwise emit invalid UTF-8, which
    # the API correctly refuses. Keep the random vision/transformer tensors
    # intact, but make this wire-test head select printable A/B tokens.
    head = weights["lm_head.weight"]
    direction = head[65].clone()
    head.zero_(); head[65] = direction; head[66] = -direction
    weights.update(vision_weights)
    shutil.copytree(args.text, args.out)
    # The copied text-only oracle no longer describes this test vocabulary/head.
    (args.out / "ref.json").unlink()
    save_file(weights, args.out / "model.safetensors")
    config = {"model_type": "qwen4_exp", "text_config": text_config, "vision_config": visual_config,
              "image_token_id": 257}
    (args.out / "config.json").write_text(json.dumps(config, indent=2) + "\n")
    tokenizer = json.loads((args.out / "tokenizer.json").read_text())
    tokenizer["added_tokens"] = [{"id": i, "content": token, "single_word": False, "lstrip": False,
        "rstrip": False, "normalized": False, "special": True} for i, token in
        ((256, "<|vision_start|>"), (257, "<|image_pad|>"), (258, "<|vision_end|>"))]
    (args.out / "tokenizer.json").write_text(json.dumps(tokenizer) + "\n")
    model = Qwen4ExpVisionModel(Qwen4ExpVisionConfig(**visual_config)).eval()
    model.load_state_dict({key.removeprefix("model.visual."): value for key, value in vision_weights.items()})
    spec = importlib.util.spec_from_file_location("reference", args.engine / "tools/qwen38_image.py")
    reference = importlib.util.module_from_spec(spec); spec.loader.exec_module(reference)
    for number in range(2):
        y, x = np.indices((256, 256))
        image = np.stack([(x * (number+1) + y * 3) % 256, (x * 5 + number*80) % 256, (y * 7 + number*40) % 256], axis=-1).astype(np.uint8)
        image_path = args.out / f"image-{number}.png"
        Image.fromarray(image).save(image_path)
        # Same patch-order reference already tested independently by image_prepare_test.
        pixels = reference.preprocess(image_path, max_tokens=256)
        # Accommodate the pinned helper's documented return tuple.
        patches, gh, gw = pixels
        with torch.no_grad():
            output = model(torch.from_numpy(patches), grid_thw=torch.tensor([[1, gh, gw]])).pooler_output
        output.numpy().astype("<f4").tofile(args.out / f"image-{number}.rows")
print("Resident multimodal fixture:", args.out)
