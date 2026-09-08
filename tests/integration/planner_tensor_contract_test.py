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
    parser.add_argument("--family", choices=("deepseek_v4", "olmoe", "inkling", "kimi", "glm", "glm53", "qwen38"), required=True)
    parser.add_argument("--probe", type=Path, default=Path("./segment_budget_probe"))
    args = parser.parse_args()
    probe = args.probe.resolve()
    files = sorted(args.model.glob("*.safetensors"))
    assert files, "fixture has no safetensors shards"
    original, metadata, payload = {}, {}, 0
    for path in files:
        with path.open("rb") as f:
            length, = struct.unpack("<Q", f.read(8))
            assert length <= 8 * 1024 * 1024
            shard = json.loads(f.read(length))
            for name, tensor in shard.items():
                if name == "__metadata__":
                    continue
                assert name not in original, f"duplicate source tensor {name}"
                begin, end = tensor["data_offsets"]
                if tensor["dtype"] == "I64" and end - begin <= 512:
                    f.seek(8 + length + begin)
                    metadata[name] = f.read(end - begin)
                tensor["data_offsets"] = [payload + begin, payload + end]
                original[name] = tensor
            payload += path.stat().st_size - 8 - length
    config = json.loads((args.model / "config.json").read_text())
    layers = config.get("text_config", config)["num_hidden_layers"]

    with tempfile.TemporaryDirectory(prefix="lumabri-contract-headers-") as tmp:
        root = Path(tmp)
        shutil.copyfile(args.model / "config.json", root / "config.json")

        def check(header, accepted, label, override=None):
            raw = json.dumps(header, separators=(",", ":")).encode()
            with (root / "model.safetensors").open("wb") as f:
                f.write(struct.pack("<Q", len(raw)))
                f.write(raw)
                f.truncate(8 + len(raw) + payload)
                for name, data in metadata.items():
                    if name in header:
                        f.seek(8 + len(raw) + header[name]["data_offsets"][0])
                        f.write((override or {}).get(name, data))
            result = subprocess.run([str(probe), str(root), "0", str(layers), "128", "1"],
                                    capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == accepted, (label, result.returncode, result.stderr)

        check(original, True, "baseline header-only inventory")
        suffix = {
            "deepseek_v4": ".ffn.experts.0.w1.scale",
            "olmoe": ".mlp.experts.0.qs",
            "inkling": ".mlp.experts.gate_up_proj.qs",
            "kimi": ".block_sparse_moe.experts.0.w1.weight_scale",
            "glm": ".mlp.experts.0.gate_proj.weight",
            "glm53": ".mlp.experts.0.gate_proj.weight",
            "qwen38": ".mlp.experts.0.gate_proj.weight",
        }[args.family]
        # Upstream generators describe their actual dense/sparse layout in
        # config; do not hard-code the first sparse layer into this test.
        expert_key = next((name for name in original if name.endswith(suffix)), None)
        assert expert_key is not None, f"fixture lacks the expected expert format: {suffix}"
        norm = "model.language_model.norm.weight" if "model.language_model.norm.weight" in original else "model.norm.weight"
        if args.family == "qwen38":
            norm = "model.hyper_connection_mixer.hc_norm.weight"
        if args.family == "deepseek_v4":
            norm = "norm.weight"
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
        if args.family == "deepseek_v4":
            changed = copy.deepcopy(original)
            changed[expert_key]["data_offsets"] = [n + 1 for n in changed[expert_key]["data_offsets"]]
            check(changed, False, "non-contiguous native expert scale bank")
            changed = copy.deepcopy(original)
            changed[expert_key]["dtype"] = "U8"
            check(changed, False, "wrong native expert scale format")
            changed = copy.deepcopy(original)
            del changed["layers.0.ffn.gate.tid2eid"]
            check(changed, False, "missing token hash router")
        if args.family == "qwen38":
            scale_key = expert_key + "_scale_inv"
            if scale_key in original:
                changed = copy.deepcopy(original)
                del changed[scale_key]
                check(changed, False, "FP8 expert without block scales")
                changed = copy.deepcopy(original)
                changed[scale_key]["shape"].reverse()
                check(changed, False, "transposed partial-block scale geometry")
                changed = copy.deepcopy(original)
                changed[scale_key]["dtype"] = "I64"
                check(changed, False, "non-floating FP8 scales")
            key = next(name for name in metadata if name.endswith("ngram_heads_offsets"))
            check(original, False, "negative PLE offset", {key: b"\xff" * len(metadata[key])})
            key = next(name for name in metadata if name.endswith("ngram_heads_vocab_sizes"))
            check(original, False, "zero PLE vocabulary", {key: b"\x00" * len(metadata[key])})
            changed = copy.deepcopy(original)
            del changed[key]
            check(changed, False, "missing PLE head metadata")
    print(f"PLANNER TENSOR CONTRACT ({args.family}): PASS")


if __name__ == "__main__":
    main()
