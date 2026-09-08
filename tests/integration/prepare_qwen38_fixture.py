#!/usr/bin/env python3
"""Generate an upstream Qwen4-Exp fixture with a byte-sized chat vocabulary.

Only synthetic fixture dimensions change. Weights and oracle come from the
original Transformers model, never from Lumabri or an edited Colibri runtime.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile


def quantize_experts(model, torch):
    """Real block E4M3 payloads, with an independent dequantized HF oracle.

    Power-of-two scales keep every reconstructed E4M3 value exactly
    representable in BF16. The reference therefore sees the actual quantized
    weights, not the original unquantized model or C-generated predictions.
    """
    payload = {}
    for layer_id, layer in enumerate(model.model.layers):
        experts = layer.mlp.experts
        intermediate = model.config.moe_intermediate_size
        for expert in range(model.config.num_experts):
            matrices = (experts.gate_up_proj[expert, :intermediate],
                        experts.gate_up_proj[expert, intermediate:], experts.down_proj[expert])
            for projection, matrix in zip(("gate_proj", "up_proj", "down_proj"), matrices):
                rows, cols = matrix.shape
                raw = torch.empty((rows, cols), dtype=torch.float8_e4m3fn)
                scales = torch.empty(((rows + 127) // 128, (cols + 127) // 128), dtype=torch.float32)
                with torch.no_grad():
                    for row in range(0, rows, 128):
                        for col in range(0, cols, 128):
                            block = matrix[row:row + 128, col:col + 128].float()
                            maximum = block.abs().max().item()
                            scale = 2.0 ** math.ceil(math.log2(maximum / 448.0)) if maximum else 1.0
                            packed = (block / scale).to(torch.float8_e4m3fn)
                            reconstructed = packed.float() * scale
                            assert torch.equal(reconstructed, reconstructed.bfloat16().float())
                            raw[row:row + 128, col:col + 128] = packed
                            scales[row // 128, col // 128] = scale
                            matrix[row:row + 128, col:col + 128].copy_(reconstructed)
                name = f"model.layers.{layer_id}.mlp.experts.{expert}.{projection}.weight"
                payload[name] = raw.contiguous()
                payload[name + "_scale_inv"] = scales
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--fp8-experts", action="store_true")
    args = parser.parse_args()
    out, engine = args.out.resolve(), args.engine.resolve()
    if out.exists():
        parser.error("--out must be new; existing checkpoints are never overwritten")
    spec = importlib.util.spec_from_file_location("upstream_q38_fixture", engine / "tools/make_qwen38_tiny.py")
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)
    torch = upstream.torch
    torch.set_num_threads(2)
    _, Model, Config = upstream._classes()
    with tempfile.TemporaryDirectory(prefix="lumabri-q38-template-") as tmp:
        upstream.build(Path(tmp), emit_ref=False)
        config = Config.from_pretrained(tmp)
    config.vocab_size = 256
    config.max_position_embeddings = 2048
    if args.fp8_experts:
        # Exercise partial 128x128 blocks, not only a one-element scale bank.
        config.hidden_size = 128
        config.moe_intermediate_size = 136
    torch.manual_seed(upstream.SEED)
    model = Model(config).to(dtype=torch.bfloat16).eval()
    packed = quantize_experts(model, torch) if args.fp8_experts else None
    out.mkdir(parents=True)
    model.save_pretrained(str(out), safe_serialization=True)
    if packed:
        from safetensors.torch import load_file, save_file
        files = list(out.glob("*.safetensors"))
        assert len(files) == 1, "synthetic fixture unexpectedly became sharded"
        tensors = load_file(files[0])
        assert all(name in tensors for name in packed if not name.endswith("_scale_inv"))
        tensors.update(packed)
        save_file(tensors, files[0], metadata={"format": "pt"})
    reference = upstream._reference(model, [1, 3, 4, 5, 6], 8)
    reference["expert_encoding"] = "E4M3 128x128 blocks, BF16-exact dequantized HF oracle" if packed else "BF16"
    (out / "ref.json").write_text(json.dumps(reference, indent=2) + "\n")
    subprocess.run([sys.executable, str(engine / "tools/make_edge_tiny_tokenizer.py"),
                    str(out), "--vocab-size", "256"], check=True)
    print("Qwen3.8 synthetic checkpoint and independent oracle:", out)


if __name__ == "__main__":
    main()
