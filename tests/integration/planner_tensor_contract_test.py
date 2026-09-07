#!/usr/bin/env python3
"""Reject damaged header-only fixtures before admission, without loading weights.

The sparse files made here deliberately contain no valid model payload and must
never be used for a numerical oracle. Real inference is tested separately.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--family", choices=("inkling", "kimi", "glm", "glm53"), required=True)
    parser.add_argument("--probe", type=Path, default=Path("./segment_budget_probe"))
    args = parser.parse_args()
    probe = args.probe.resolve()
    files = list(args.model.glob("*.safetensors"))
    assert len(files) == 1, "this small-fixture test requires one shard"
    with files[0].open("rb") as f:
        length, = struct.unpack("<Q", f.read(8))
        assert length <= 8 * 1024 * 1024
        original = json.loads(f.read(length))
    payload = files[0].stat().st_size - 8 - length
    config = json.loads((args.model / "config.json").read_text())
    layers = config.get("text_config", config)["num_hidden_layers"]

    with tempfile.TemporaryDirectory(prefix="lumabri-contract-headers-") as tmp:
        root = Path(tmp)
        shutil.copyfile(args.model / "config.json", root / "config.json")

        def check(header, accepted, label):
            raw = json.dumps(header, separators=(",", ":")).encode()
            with (root / "model.safetensors").open("wb") as f:
                f.write(struct.pack("<Q", len(raw)))
                f.write(raw)
                f.truncate(8 + len(raw) + payload)
            result = subprocess.run([str(probe), str(root), "0", str(layers), "128", "1"],
                                    capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == accepted, (label, result.returncode, result.stderr)

        check(original, True, "baseline header-only inventory")
        suffix = {
            "inkling": ".mlp.experts.gate_up_proj.qs",
            "kimi": ".block_sparse_moe.experts.0.w1.weight_scale",
            "glm": ".mlp.experts.0.gate_proj.weight",
            "glm53": ".mlp.experts.0.gate_proj.weight",
        }[args.family]
        # Upstream generators describe their actual dense/sparse layout in
        # config; do not hard-code the first sparse layer into this test.
        expert_key = next((name for name in original if name.startswith("model.")
                           and name.endswith(suffix)), None)
        assert expert_key is not None, f"fixture lacks the expected expert format: {suffix}"
        norm = "model.language_model.norm.weight" if "model.language_model.norm.weight" in original else "model.norm.weight"
        for key in (norm, expert_key):
            changed = copy.deepcopy(original)
            assert key in changed, f"fixture changed: missing {key}"
            del changed[key]
            check(changed, False, f"missing {key}")
        changed = copy.deepcopy(original)
        changed[norm]["data_offsets"][1] = payload + 1
        check(changed, False, "tensor outside payload")
        changed = copy.deepcopy(original)
        changed[norm]["shape"][-1] += 1
        check(changed, False, "wrong tensor geometry")
        if args.family == "glm":
            changed = copy.deepcopy(original)
            del changed["model.layers.0.self_attn.indexer.wk.weight"]
            check(changed, False, "partial DSA bank with wq still present")
    print(f"PLANNER TENSOR CONTRACT ({args.family}): PASS")


if __name__ == "__main__":
    main()
