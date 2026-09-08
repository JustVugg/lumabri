#!/usr/bin/env python3
"""Write a sparse HEADER-ONLY OLMoE fixture. Not usable for inference/oracles."""
import argparse
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    config = json.loads((args.root / "config.json").read_text())
    h, inter, experts, vocab = (config[k] for k in
        ("hidden_size", "intermediate_size", "num_experts", "vocab_size"))
    header, size = {}, 0

    def tensor(name, shape, dtype="F32"):
        nonlocal size
        count = 1
        for n in shape:
            count *= n
        end = size + count * (4 if dtype == "F32" else 1)
        header[name] = dict(dtype=dtype, shape=shape, data_offsets=[size, end])
        size = end

    tensor("model.embed_tokens.weight", [vocab, h])
    tensor("lm_head.weight", [vocab, h])
    tensor("model.norm.weight", [h])
    for layer in range(config["num_hidden_layers"]):
        prefix = f"model.layers.{layer}."
        for suffix in ("input_layernorm.weight", "post_attention_layernorm.weight",
                       "self_attn.q_norm.weight", "self_attn.k_norm.weight"):
            tensor(prefix + suffix, [h])
        for proj in "qkvo":
            tensor(prefix + f"self_attn.{proj}_proj.weight", [h, h])
        tensor(prefix + "mlp.gate.weight", [experts, h])
        for expert in range(experts):
            ep = prefix + f"mlp.experts.{expert}."
            tensor(ep + "merged_weight", [3 * inter * h], "I8")
            tensor(ep + "qs", [2 * inter + h])
    raw = json.dumps(header, separators=(",", ":")).encode()
    path = args.root / "model.safetensors"
    with path.open("xb") as f:
        f.write(struct.pack("<Q", len(raw)))
        f.write(raw)
        f.truncate(8 + len(raw) + size)


if __name__ == "__main__":
    main()
