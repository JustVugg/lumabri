#!/usr/bin/env python3
"""Generate a small Kimi-K3-shaped model for the lumabri phase-2 test.

Random weights: the question is not text quality, it is whether the SAME
engine produces the same tokens with its experts local and with them on
peers. numpy only.

K3's two peculiarities, both of which the fixture has to reproduce or the
test proves nothing:

  · the routed experts work in a LATENT space (routed_expert_hidden_size),
    not in hidden — the layer projects down, routes and runs there, then
    projects back up. That latent is the width lumabri puts on the wire.
  · the expert weights are native MXFP4: 4-bit e2m1 pairs packed two per
    byte, with one e8m0 exponent byte per group of 32. The engine checks the
    byte counts exactly and refuses a container that does not match, so the
    six tensors per expert are emitted at precisely
      w1/w3 packed [moe_inter, latent/2]      scale [moe_inter, latent/32]
      w2    packed [latent, moe_inter/2]      scale [latent, moe_inter/32]

The values inside are random: every e2m1 code is finite, and the exponent
bytes are kept near 127 (2^0) so nothing overflows. Everything else is f32
and loaded with K3_BITS=32, which keeps the dense side exact and out of the
way of the question being asked.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

H = 128          # hidden
L = 6            # layers
V = 256          # vocab
FIRST_DENSE = 2
DENSE_I = 64
NH = 4           # attention heads
Q_LORA = 32
KV_LORA = 32
QK_NOPE = 16
QK_ROPE = 8
V_HEAD = 16
E = 8            # routed experts
TOPK = 2
MOE_I = 32       # multiple of 32
LATENT = 32      # multiple of 32
N_SHARED = 1
RES_BS = 2
KDA_HEADS = 2
KDA_HD = 16
CONV_K = 4
KDA_LAYERS = [1, 3]              # 1-indexed, per the engine's config schema

CFG = {
    "model_type": "kimi_k3",
    "architectures": ["KimiLinearForCausalLM"],
    "hidden_size": H,
    "num_hidden_layers": L,
    "vocab_size": V,
    "first_k_dense_replace": FIRST_DENSE,
    "intermediate_size": DENSE_I,
    "num_attention_heads": NH,
    "q_lora_rank": Q_LORA,
    "kv_lora_rank": KV_LORA,
    "qk_nope_head_dim": QK_NOPE,
    "qk_rope_head_dim": QK_ROPE,
    "v_head_dim": V_HEAD,
    "num_experts": E,
    "num_experts_per_token": TOPK,
    "moe_intermediate_size": MOE_I,
    "routed_expert_hidden_size": LATENT,
    "num_shared_experts": N_SHARED,
    "attn_res_block_size": RES_BS,
    "activation_situ_beta": 1.0,
    "activation_situ_linear_beta": 1.0,
    "rms_norm_eps": 1e-5,
    "linear_attn_config": {
        "num_heads": KDA_HEADS,
        "head_dim": KDA_HD,
        "short_conv_kernel_size": CONV_K,
        "kda_layers": KDA_LAYERS,
        "gate_lower_bound": -5.0,
    },
    "bos_token_id": 0,
    "eos_token_id": 1,
}

DTYPE = {np.dtype("float32"): "F32", np.dtype("uint8"): "U8"}


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "tiny_kimi")
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20260807)

    P = KDA_HEADS * KDA_HD           # kda projection width
    QK_HEAD = QK_NOPE + QK_ROPE
    SHI = MOE_I * N_SHARED
    tensors = {}

    def put(name, arr):
        tensors[name] = np.ascontiguousarray(arr)

    def f(*shape, s=0.02):
        return (rng.standard_normal(shape) * s).astype(np.float32)

    def ones(n):
        return np.ones(n, dtype=np.float32)

    def mxfp4(rows, cols):
        """one MXFP4 matrix: packed nibble pairs + one e8m0 byte per 32."""
        packed = rng.integers(0, 256, size=(rows, cols // 2), dtype=np.uint8)
        # exponents near 127 (2^0): tame magnitudes, never the 0xFF NaN code
        scale = rng.integers(124, 131, size=(rows, cols // 32), dtype=np.uint8)
        return packed, scale

    put("model.embed_tokens.weight", f(V, H))
    put("model.norm.weight", ones(H))
    put("model.output_attn_res_norm.weight", ones(H))
    put("model.output_attn_res_proj.weight", f(H, s=0.1))
    put("lm_head.weight", f(V, H))

    for i in range(L):
        p = f"model.layers.{i}."
        put(p + "input_layernorm.weight", ones(H))
        put(p + "post_attention_layernorm.weight", ones(H))
        put(p + "self_attention_res_norm.weight", ones(H))
        put(p + "self_attention_res_proj.weight", f(H, s=0.1))
        put(p + "mlp_res_norm.weight", ones(H))
        put(p + "mlp_res_proj.weight", f(H, s=0.1))

        if (i + 1) in KDA_LAYERS:                    # kda_layers is 1-indexed
            a = p + "self_attn."
            for n in ("q_proj", "k_proj", "v_proj", "g_proj"):
                put(a + n + ".weight", f(P, H, s=0.05))
            put(a + "o_proj.weight", f(H, P, s=0.05))
            for n in ("q_conv1d", "k_conv1d", "v_conv1d"):
                put(a + n + ".weight", f(P, CONV_K, s=0.3))
            put(a + "f_a_proj.weight", f(KDA_HD, H, s=0.05))
            put(a + "f_b_proj.weight", f(P, KDA_HD, s=0.05))
            put(a + "b_proj.weight", f(KDA_HEADS, H, s=0.05))
            put(a + "dt_bias", f(P, s=0.1))
            put(a + "o_norm.weight", ones(KDA_HD))
            # A_log is stored per-head zero-padded to kda_hd; the engine reads
            # the first kda_heads entries and takes exp() of them
            put(a + "A_log", f(KDA_HD, s=0.1))
        else:
            a = p + "self_attn."
            put(a + "q_a_proj.weight", f(Q_LORA, H, s=0.05))
            put(a + "q_b_proj.weight", f(NH * QK_HEAD, Q_LORA, s=0.05))
            put(a + "kv_a_proj_with_mqa.weight", f(KV_LORA + QK_ROPE, H, s=0.05))
            put(a + "kv_b_proj.weight", f(NH * (QK_NOPE + V_HEAD), KV_LORA, s=0.05))
            put(a + "o_proj.weight", f(H, NH * V_HEAD, s=0.05))
            put(a + "g_proj.weight", f(NH * V_HEAD, H, s=0.05))
            put(a + "q_a_layernorm.weight", ones(Q_LORA))
            put(a + "kv_a_layernorm.weight", ones(KV_LORA))

        if i < FIRST_DENSE:
            put(p + "mlp.gate_proj.weight", f(DENSE_I, H, s=0.05))
            put(p + "mlp.up_proj.weight", f(DENSE_I, H, s=0.05))
            put(p + "mlp.down_proj.weight", f(H, DENSE_I, s=0.05))
        else:
            b = p + "block_sparse_moe."
            put(b + "gate.weight", f(E, H, s=0.05))
            put(b + "gate.e_score_correction_bias", f(E, s=0.01))
            put(b + "routed_expert_norm.weight", ones(LATENT))
            put(b + "routed_expert_down_proj.weight", f(LATENT, H, s=0.05))
            put(b + "routed_expert_up_proj.weight", f(H, LATENT, s=0.05))
            put(b + "shared_experts.gate_proj.weight", f(SHI, H, s=0.05))
            put(b + "shared_experts.up_proj.weight", f(SHI, H, s=0.05))
            put(b + "shared_experts.down_proj.weight", f(H, SHI, s=0.05))
            for e in range(E):
                q = b + f"experts.{e}."
                # emitted w1,w2,w3 packed/scale in the order the engine reads
                w1p, w1s = mxfp4(MOE_I, LATENT)
                w2p, w2s = mxfp4(LATENT, MOE_I)
                w3p, w3s = mxfp4(MOE_I, LATENT)
                put(q + "w1.weight_packed", w1p); put(q + "w1.weight_scale", w1s)
                put(q + "w2.weight_packed", w2p); put(q + "w2.weight_scale", w2s)
                put(q + "w3.weight_packed", w3p); put(q + "w3.weight_scale", w3s)

    header, offset = {}, 0
    for name, arr in tensors.items():
        header[name] = {"dtype": DTYPE[arr.dtype], "shape": list(arr.shape),
                        "data_offsets": [offset, offset + arr.nbytes]}
        offset += arr.nbytes
    blob = json.dumps(header, separators=(",", ":")).encode()
    blob += b" " * ((-len(blob)) % 8)

    with open(out / "model.safetensors", "wb") as fh:
        fh.write(struct.pack("<Q", len(blob)))
        fh.write(blob)
        for arr in tensors.values():
            fh.write(arr.tobytes())

    (out / "config.json").write_text(json.dumps(CFG, indent=2))
    total = sum(a.nbytes for a in tensors.values())
    print(f"{out}: {len(tensors)} tensors, {total/1e6:.1f} MB, "
          f"{L} layers ({L-FIRST_DENSE} sparse) x {E} experts, latent {LATENT}")


if __name__ == "__main__":
    main()
