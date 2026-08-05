#!/usr/bin/env python3
"""Generate a small OLMoE-shaped model for the lumabri phase-2 prototype.

Random weights: the point is not text quality, it is that local execution and
remote (peer) execution of the very same experts produce the very same tokens.
The shapes are chosen so the timing means something — per-expert size and layer
count are the two quantities that set the P2P round-trip budget:

  hidden 1024, inter 1024  → one expert = 3·1024·1024 int8 ≈ 3.1 MB
  16 layers × 8 experts, top-4  → 64 remote expert calls per token

Real OLMoE-1B-7B is hidden 2048 / inter 1024 / 16 layers / 64 experts / top-8:
same layer count, expert twice as wide, twice as many calls per token.

Container format is what olmoe.c's loader expects:
  model.layers.L.mlp.experts.E.merged_weight  I8  [3·inter·hidden]  (gate,up,down)
  model.layers.L.mlp.experts.E.qs             F32 [inter+inter+hidden] per-row scales
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

BASE = {
    "architectures": ["OlmoeForCausalLM"],
    "num_hidden_layers": 16,
    "num_attention_heads": 16,
    "num_key_value_heads": 16,
    "vocab_size": 1024,
    "rope_theta": 10000.0,
    "rms_norm_eps": 1e-5,
    "norm_topk_prob": True,
    "model_type": "olmoe",
}

# "small" is the correctness fixture: fast to build, cheap on disk.
# "real" matches OLMoE-1B-7B where it changes the P2P economics — the expert
# is the true 6.3 MB and top-8 means the true 128 remote calls per token. Only
# the expert POOL is smaller (16 instead of 64), which changes cache pressure,
# not the per-token round-trip budget.
PRESETS = {
    "small": {"hidden_size": 1024, "intermediate_size": 1024,
              "num_experts": 8, "num_experts_per_tok": 4},
    "real":  {"hidden_size": 2048, "intermediate_size": 1024,
              "num_experts": 16, "num_experts_per_tok": 8},
}

DTYPE = {np.dtype("float32"): "F32", np.dtype("int8"): "I8"}


def quantize_rows(w):
    """int8 per-row + f32 scale — the format olmoe.c's matmul_q dequants."""
    scale = np.abs(w).max(axis=1) / 127.0
    scale[scale < 1e-12] = 1e-12
    q = np.rint(w / scale[:, None]).clip(-127, 127).astype(np.int8)
    return q, scale.astype(np.float32)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "tiny_olmoe")
    preset = sys.argv[2] if len(sys.argv) > 2 else "small"
    if preset not in PRESETS:
        sys.exit(f"preset must be one of {list(PRESETS)}")
    CFG = dict(BASE, **PRESETS[preset])
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20260804)

    D = CFG["hidden_size"]
    I = CFG["intermediate_size"]
    L = CFG["num_hidden_layers"]
    E = CFG["num_experts"]
    V = CFG["vocab_size"]

    tensors = {}

    def put(name, arr):
        tensors[name] = np.ascontiguousarray(arr)

    def normal(*shape, s=0.02):
        return (rng.standard_normal(shape) * s).astype(np.float32)

    put("model.embed_tokens.weight", normal(V, D))
    put("lm_head.weight", normal(V, D))
    put("model.norm.weight", np.ones(D, dtype=np.float32))

    for l in range(L):
        p = f"model.layers.{l}."
        put(p + "input_layernorm.weight", np.ones(D, dtype=np.float32))
        put(p + "post_attention_layernorm.weight", np.ones(D, dtype=np.float32))
        for proj in ("q_proj", "k_proj", "v_proj", "o_proj"):
            put(p + f"self_attn.{proj}.weight", normal(D, D, s=0.03))
        put(p + "self_attn.q_norm.weight", np.ones(D, dtype=np.float32))
        put(p + "self_attn.k_norm.weight", np.ones(D, dtype=np.float32))
        put(p + "mlp.gate.weight", normal(E, D, s=0.05))
        for e in range(E):
            gate_w = normal(I, D, s=0.05)
            up_w = normal(I, D, s=0.05)
            down_w = normal(D, I, s=0.05)
            qg, sg = quantize_rows(gate_w)
            qu, su = quantize_rows(up_w)
            qd, sd = quantize_rows(down_w)
            put(p + f"mlp.experts.{e}.merged_weight",
                np.concatenate([qg.reshape(-1), qu.reshape(-1), qd.reshape(-1)]))
            put(p + f"mlp.experts.{e}.qs", np.concatenate([sg, su, sd]))

    # ---- write one safetensors shard --------------------------------------
    header, offset = {}, 0
    for name, arr in tensors.items():
        nbytes = arr.nbytes
        header[name] = {"dtype": DTYPE[arr.dtype], "shape": list(arr.shape),
                        "data_offsets": [offset, offset + nbytes]}
        offset += nbytes
    blob = json.dumps(header, separators=(",", ":")).encode()
    pad = (-len(blob)) % 8                      # keep the data 8-byte aligned
    blob += b" " * pad

    path = out / "model.safetensors"
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(blob)))
        f.write(blob)
        for arr in tensors.values():
            f.write(arr.tobytes())

    (out / "config.json").write_text(json.dumps(CFG, indent=2))

    # ---- byte-level tokenizer -------------------------------------------
    # CHAT mode needs a tokenizer.json. A byte-level vocabulary (the GPT-2
    # byte↔unicode mapping tok.h already speaks, no merges) is enough: every
    # message encodes byte by byte, and ids stay well under vocab_size. The
    # three template strings ride along as atomic added_tokens so the chat
    # template and the eos check in run_chat work exactly as with a real model.
    def bytes_to_unicode():
        bs = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
        cs = bs[:]
        n = 0
        for b in range(256):
            if b not in bs:
                bs.append(b)
                cs.append(256 + n)
                n += 1
        return {b: chr(c) for b, c in zip(bs, cs)}

    b2u = bytes_to_unicode()
    vocab = {b2u[b]: b for b in range(256)}
    added = [{"id": 256 + i, "content": s, "single_word": False, "lstrip": False,
              "rstrip": False, "normalized": False, "special": True}
             for i, s in enumerate(["|||IP_ADDRESS|||", "<|user|>", "<|assistant|>"])]
    (out / "tokenizer.json").write_text(json.dumps({
        "version": "1.0",
        "added_tokens": added,
        "model": {"type": "BPE", "vocab": vocab, "merges": []},
    }, ensure_ascii=False))

    # The engine's ref.json harness: it generates len(full)-len(prompt) tokens
    # greedily and prints them. With random weights the "reference" tail is
    # meaningless — what the experiment compares is the local run against the
    # remote run, token by token.
    n_new = 24
    prompt = [int(x) for x in rng.integers(0, V, size=8)]
    (out / "ref.json").write_text(json.dumps(
        {"prompt_ids": prompt, "full_ids": prompt + [0] * n_new}, indent=2))
    total = path.stat().st_size
    experts = L * E
    print(f"{out}: {len(tensors)} tensors, {total/1e6:.0f} MB")
    print(f"  {L} layers x {E} experts = {experts} experts, "
          f"{3*I*D/1e6:.1f} MB each, top-{CFG['num_experts_per_tok']}")
    print(f"  → {L*CFG['num_experts_per_tok']} remote expert calls per token")


if __name__ == "__main__":
    main()
