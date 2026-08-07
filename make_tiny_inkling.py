#!/usr/bin/env python3
"""Generate a small Inkling-shaped model for the lumabri phase-2 test.

colibri ships tools/make_tiny_inkling.py, which builds a real oracle through
HF transformers — but transformers only grew Inkling support recently, and
the fixture here does not need an oracle at all. What the phase-2 test asks
is narrower: run the SAME engine twice, once with the experts local and once
with them on peers, and demand the same tokens. Random weights answer that
question perfectly, and numpy is the only dependency.

The shapes are the ones inkling.c's loader and forward actually read:

  hidden D, heads H·hd (swa_* on the sliding layers), kv KV·hd
  r_proj             [H·d_rel, D]      relative-logit projection
  rel_logits_proj    [d_rel, extent]   extent = window | rel_extent per layer
  {k,v}_sconv        [KV·hd, conv_k]   short convs over the raw projections
  {attn,mlp}_sconv   [D, conv_k]
  experts.gate_up_proj  [E, 2·I, D]    f32 — the loader's non-container path
  experts.down_proj     [E, D, I]

f32 experts on purpose: the packed container path (m->xq) would need the
colibri container layout, and the P2P question is about where the expert
runs, not how it is quantized.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

CFG = {
    "architectures": ["InklingForCausalLM"],
    "model_type": "inkling",
    "hidden_size": 128,
    "num_hidden_layers": 6,
    "vocab_size": 256,
    "unpadded_vocab_size": 250,
    "num_attention_heads": 4,
    "num_key_value_heads": 2,
    "head_dim": 16,
    "swa_num_attention_heads": 4,
    "swa_num_key_value_heads": 2,
    "swa_head_dim": 16,
    "sliding_window_size": 32,
    "d_rel": 8,
    "rel_extent": 64,
    "sconv_kernel_size": 4,
    "n_routed_experts": 8,
    "num_experts_per_tok": 2,
    "n_shared_experts": 1,
    "intermediate_size": 64,          # dense MLP
    "moe_intermediate_size": 32,      # MoE expert
    "rms_norm_eps": 1e-5,
    "route_scale": 8.0,
    "logits_mup_width_multiplier": 1.0,
    "rope_theta": 10000.0,
    "eos_token_id": 1,
    # layers 0-1 dense, 2-5 sparse; sliding on all but every 6th (the engine's
    # own default rule, written out so the fixture does not depend on it)
    "mlp_layer_types": ["dense", "dense", "sparse", "sparse", "sparse", "sparse"],
    "layer_types": ["hybrid_sliding", "hybrid_sliding", "hybrid_sliding",
                    "hybrid_sliding", "hybrid_sliding", "full_attention"],
}

DTYPE = {np.dtype("float32"): "F32"}


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "tiny_inkling")
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20260807)

    D = CFG["hidden_size"]
    L = CFG["num_hidden_layers"]
    V = CFG["vocab_size"]
    E = CFG["n_routed_experts"]
    NS = CFG["n_shared_experts"]
    I = CFG["moe_intermediate_size"]
    DI = CFG["intermediate_size"]
    K = CFG["sconv_kernel_size"]
    DR = CFG["d_rel"]

    tensors = {}

    def put(name, arr):
        tensors[name] = np.ascontiguousarray(arr, dtype=np.float32)

    def normal(*shape, s=0.02):
        return (rng.standard_normal(shape) * s).astype(np.float32)

    put("model.embed_tokens.weight", normal(V, D))
    put("model.embed_norm.weight", np.ones(D, dtype=np.float32))
    put("model.norm.weight", np.ones(D, dtype=np.float32))
    put("lm_head.weight", normal(V, D))

    for l in range(L):
        p = f"model.layers.{l}."
        local = CFG["layer_types"][l] == "hybrid_sliding"
        H = CFG["swa_num_attention_heads"] if local else CFG["num_attention_heads"]
        KV = CFG["swa_num_key_value_heads"] if local else CFG["num_key_value_heads"]
        hd = CFG["swa_head_dim"] if local else CFG["head_dim"]
        ext = CFG["sliding_window_size"] if local else CFG["rel_extent"]
        qdim, kvdim = H * hd, KV * hd

        put(p + "input_layernorm.weight", np.ones(D, dtype=np.float32))
        put(p + "post_attention_layernorm.weight", np.ones(D, dtype=np.float32))
        put(p + "self_attn.q_proj.weight", normal(qdim, D, s=0.05))
        put(p + "self_attn.k_proj.weight", normal(kvdim, D, s=0.05))
        put(p + "self_attn.v_proj.weight", normal(kvdim, D, s=0.05))
        put(p + "self_attn.r_proj.weight", normal(H * DR, D, s=0.05))
        put(p + "self_attn.o_proj.weight", normal(D, qdim, s=0.05))
        put(p + "self_attn.q_norm.weight", np.ones(hd, dtype=np.float32))
        put(p + "self_attn.k_norm.weight", np.ones(hd, dtype=np.float32))
        put(p + "self_attn.rel_logits_proj.proj", normal(DR, ext, s=0.05))
        put(p + "self_attn.k_sconv.conv1d.weight", normal(kvdim, K, s=0.3))
        put(p + "self_attn.v_sconv.conv1d.weight", normal(kvdim, K, s=0.3))
        put(p + "attn_sconv.conv1d.weight", normal(D, K, s=0.3))
        put(p + "mlp_sconv.conv1d.weight", normal(D, K, s=0.3))

        if CFG["mlp_layer_types"][l] == "dense":
            put(p + "mlp.gate_proj.weight", normal(DI, D, s=0.05))
            put(p + "mlp.up_proj.weight", normal(DI, D, s=0.05))
            put(p + "mlp.down_proj.weight", normal(D, DI, s=0.05))
            put(p + "mlp.global_scale", np.array(1.0, dtype=np.float32))
        else:
            # router scores routed experts AND the shared ones: E + NS rows
            put(p + "mlp.gate.weight", normal(E + NS, D, s=0.05))
            put(p + "mlp.gate.e_score_correction_bias", normal(E, s=0.01))
            put(p + "mlp.gate.global_scale", np.array(1.0, dtype=np.float32))
            put(p + "mlp.shared_experts.gate_proj", normal(NS * I, D, s=0.05))
            put(p + "mlp.shared_experts.up_proj", normal(NS * I, D, s=0.05))
            put(p + "mlp.shared_experts.down_proj", normal(D, NS * I, s=0.05))
            put(p + "mlp.experts.gate_up_proj", normal(E, 2 * I, D, s=0.05))
            put(p + "mlp.experts.down_proj", normal(E, D, I, s=0.05))

    header, offset = {}, 0
    for name, arr in tensors.items():
        header[name] = {"dtype": DTYPE[arr.dtype], "shape": list(arr.shape),
                        "data_offsets": [offset, offset + arr.nbytes]}
        offset += arr.nbytes
    blob = json.dumps(header, separators=(",", ":")).encode()
    blob += b" " * ((-len(blob)) % 8)

    with open(out / "model.safetensors", "wb") as f:
        f.write(struct.pack("<Q", len(blob)))
        f.write(blob)
        for arr in tensors.values():
            f.write(arr.tobytes())

    (out / "config.json").write_text(json.dumps(CFG, indent=2))
    total = sum(a.nbytes for a in tensors.values())
    print(f"{out}: {len(tensors)} tensors, {total/1e6:.1f} MB, "
          f"{L} layers ({CFG['mlp_layer_types'].count('sparse')} sparse) x {E} experts")


if __name__ == "__main__":
    main()
