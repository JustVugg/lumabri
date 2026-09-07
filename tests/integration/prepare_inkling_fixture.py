#!/usr/bin/env python3
"""Build an isolated synthetic Inkling fixture with the pinned engine's tag.

Transformers exports `inkling_text`; the pinned Colibri registry accepts
`inkling`/`inkling_mm_model`. Only this synthetic fixture is retagged. Runtime
model identification remains exact and does not guess aliases for user models.
No Colibri source or user checkpoint is modified.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import struct


def verify_int4(model):
    """A successful converter exit is not evidence of quantization."""
    config = json.loads((model / "config.json").read_text())
    headers = {}
    for shard in model.glob("*.safetensors"):
        with shard.open("rb") as f:
            size, = struct.unpack("<Q", f.read(8))
            headers.update(json.loads(f.read(size)))
    experts = config["n_routed_experts"]
    hidden, inter = config["hidden_size"], config["moe_intermediate_size"]
    sparse = 0
    for layer, kind in enumerate(config["mlp_layer_types"]):
        if kind != "sparse":
            continue
        sparse += 1
        for suffix, rows, cols in (("gate_up_proj", 2 * inter, hidden),
                                   ("down_proj", hidden, inter)):
            key = f"model.layers.{layer}.mlp.experts.{suffix}"
            tensor, scale = headers[key], headers[key + ".qs"]
            assert tensor["dtype"] == "U8", f"not packed int4: {key}"
            assert tensor["data_offsets"][1] - tensor["data_offsets"][0] == experts * rows * cols // 2
            assert scale["dtype"] == "F32"
            assert scale["data_offsets"][1] - scale["data_offsets"][0] == experts * rows * 4
    assert sparse > 0, "fixture must exercise routed experts"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    engine = args.engine.resolve()
    out = args.out.resolve()
    if out.exists():
        parser.error("--out must be a new directory; existing fixtures are never overwritten")
    out.mkdir(parents=True)
    env = dict(os.environ, OMP_NUM_THREADS="2", MKL_NUM_THREADS="2")
    source = out / "float" / "inkling-tiny"
    subprocess.run([sys.executable, str(engine / "tools/make_tiny_inkling.py"),
                    str(source)], check=True, env=env)
    config_path = source / "config.json"
    config = json.loads(config_path.read_text())
    if config["model_type"] != "inkling_text":
        raise RuntimeError("upstream fixture changed; review its registry mapping")
    config["model_type"] = "inkling"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    subprocess.run([sys.executable, str(engine / "tools/make_edge_tiny_tokenizer.py"),
                    str(source), "--vocab-size", "256"], check=True, env=env)
    # This converter quantizes original TML names, not HF expert names. Its
    # upstream e2e fixture helper reverses the mapping before conversion.
    converted = out / "converted"
    subprocess.run([sys.executable, str(engine / "tools/convert_inkling_int4.py"),
                    "--selftest-e2e", str(source), str(converted)], check=True, env=env)
    packed = out / "int4" / "inkling-tiny"
    packed.parent.mkdir()
    Path(str(converted) + "-i4").rename(packed)
    verify_int4(packed)
    subprocess.run([sys.executable, str(engine / "tools/make_edge_tiny_tokenizer.py"),
                    str(packed), "--vocab-size", "256"], check=True, env=env)


if __name__ == "__main__":
    main()
