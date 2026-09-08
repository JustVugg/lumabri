#ifndef LUMABRI_PLAN_INKLING_H
#define LUMABRI_PLAN_INKLING_H
#include "tensors.h"

static int lmb_plan_optional(const char *json, const char *key, uint32_t fallback,
                              uint32_t low, uint32_t high, uint32_t *out) {
    if (!lmb_json_member(json, key)) { *out = fallback; return 0; }
    return lmb_plan_u32(json, key, low, high, out);
}

static int lmb_plan_kinds(const char *json, const char *key, const char *zero,
                           const char *one, uint8_t *values, uint32_t count) {
    const char *p = lmb_json_member(json, key);
    if (!p || *p++ != '[') return -1;
    for (uint32_t i = 0; i < count; i++) {
        char kind[64];
        if (!(p = lmb_plan_string(lmb_plan_space(p), kind, sizeof kind))) return -1;
        if (!strcmp(kind, zero)) values[i] = 0;
        else if (!strcmp(kind, one)) values[i] = 1;
        else return -1;
        p = lmb_plan_space(p);
        if (*p++ != (i + 1 == count ? ']' : ',')) return -1;
    }
    return 0;
}

typedef struct {
    LmbModelShape *model;
    uint8_t sparse[256], local[256], packed[256];
    uint8_t encoding[256][2];
    uint32_t seen[256], edge_seen;
    uint32_t hidden, inter, experts;
    int sidecar;
} LmbInklingInventory;

static int lmb_inkling_tensor(const LmbPlanTensor *t, void *opaque) {
    if (!strcmp(t->dtype, "I64")) return -1;
    LmbInklingInventory *v = opaque;
    LmbModelShape *m = v->model;
    uint64_t cost;
    int packed = !strcmp(t->dtype, "U8") || !strcmp(t->dtype, "I8");
    const char *name = t->name;
    int global = !strcmp(name, "model.embed_tokens.weight") ? 0 :
        !strcmp(name, "lm_head.weight") ? 1 : !strcmp(name, "model.norm.weight") ? 2 :
        !strcmp(name, "model.embed_norm.weight") ? 3 : -1;
    if (global >= 0) {
        if (!v->sidecar && (v->edge_seen & (1u << global))) return -1;
        if (!v->sidecar) v->edge_seen |= 1u << global;
        if (!v->sidecar && (packed || t->elements !=
            (global < 2 ? (uint64_t)m->vocab * m->hidden : m->hidden))) return -1;
        cost = global < 2 && !strcmp(t->dtype, "BF16") ? t->bytes :
            packed ? t->bytes : lmb_size_mul(t->elements, 4);
        m->edge_resident_bytes = lmb_size_add(m->edge_resident_bytes, cost);
        return m->edge_resident_bytes == UINT64_MAX ? -1 : 0;
    }
    if (strncmp(name, "model.layers.", 13)) {
        if (v->sidecar) {
            cost = packed ? t->bytes : lmb_size_mul(t->elements, 4);
            m->edge_resident_bytes = lmb_size_add(m->edge_resident_bytes, cost);
            if (m->edge_resident_bytes == UINT64_MAX) return -1;
        }
        return 0;
    }
    uint64_t layer;
    const char *suffix = lmb_plan_uint(name + 13, &layer);
    if (!suffix || *suffix++ != '.') return -1;
    if (layer >= m->layers) return 0; /* unexecuted MTP/auxiliary tensors */
    static const char *const fields[] = {
        "input_layernorm.weight", "post_attention_layernorm.weight",
        "self_attn.q_proj.weight", "self_attn.k_proj.weight", "self_attn.v_proj.weight",
        "self_attn.r_proj.weight", "self_attn.o_proj.weight", "self_attn.q_norm.weight",
        "self_attn.k_norm.weight", "self_attn.rel_logits_proj.proj",
        "self_attn.k_sconv.conv1d.weight", "self_attn.v_sconv.conv1d.weight",
        "attn_sconv.conv1d.weight", "mlp_sconv.conv1d.weight",
        "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight",
        "mlp.gate.weight", "mlp.gate.e_score_correction_bias",
        "mlp.shared_experts.gate_proj", "mlp.shared_experts.up_proj", "mlp.shared_experts.down_proj",
        "mlp.experts.gate_up_proj", "mlp.experts.down_proj",
        "mlp.experts.gate_up_proj.qs", "mlp.experts.down_proj.qs"
    };
    unsigned field;
    for (field = 0; field < sizeof fields / sizeof *fields; field++)
        if (!strcmp(suffix, fields[field])) break;
    if (v->sidecar) {
        /* Optional dense quantization may fall back per tensor. Reserve both
         * banks, rather than assuming the compressed tensor will be selected. */
        cost = packed ? t->bytes : lmb_size_mul(t->elements, 4);
    } else {
        if (field == sizeof fields / sizeof *fields) return 0;
        if (v->seen[layer] & (1u << field)) return -1;
        v->seen[layer] |= 1u << field;
        int matrix = (field >= 2 && field <= 6) || (field >= 14 && field <= 16) ||
                     (field >= 19 && field <= 21);
        if (field == 22 || field == 23) {
            uint64_t expected = (uint64_t)v->experts * v->inter * v->hidden * (field == 22 ? 2u : 1u);
            if ((!packed && t->elements != expected) ||
                (packed && t->bytes != expected && t->bytes != expected / 2)) return -1;
            if (packed) {
                cost = t->bytes;
                v->packed[layer] |= field == 22 ? 1u : 2u;
                v->encoding[layer][field == 22 ? 0 : 1] = t->bytes == expected ? 8 : 4;
            } else {
                /* Float source may be loaded exact or quantized by an explicit
                 * runtime profile; include f32 plus possible row scales. */
                uint64_t rows = (uint64_t)v->experts * (field == 22 ? 2u * v->inter : v->hidden);
                cost = lmb_size_add(lmb_size_mul(t->elements, 4), rows * 4);
            }
        } else {
            if (packed) return -1;
            if ((field == 24 || field == 25) && t->elements !=
                (uint64_t)v->experts * (field == 24 ? 2u * v->inter : v->hidden)) return -1;
            cost = matrix && !strcmp(t->dtype, "BF16") ? t->bytes : lmb_size_mul(t->elements, 4);
        }
    }
    m->memory[layer].resident_bytes = lmb_size_add(m->memory[layer].resident_bytes, cost);
    return m->memory[layer].resident_bytes == UINT64_MAX ? -1 : 0;
}

static int LMB_UNUSED lmb_inkling_memory(const char *root, const char *json, LmbModelShape *m) {
    LmbInklingInventory v = { .model = m };
    uint32_t h, layers, heads, kv, hd, sh, sk, sd, window, rel, extent, conv, experts, topk;
    uint32_t shared, dense, inter, first, unpadded;
#define NEED(key, field, lo, hi) if (lmb_plan_u32(json, key, lo, hi, &field)) return -1
#define OPT(key, field, def, lo, hi) if (lmb_plan_optional(json, key, def, lo, hi, &field)) return -1
    NEED("hidden_size", h, 1, 65536); NEED("num_hidden_layers", layers, 1, 256);
    NEED("num_attention_heads", heads, 1, 65536); NEED("num_key_value_heads", kv, 1, 65536);
    NEED("head_dim", hd, 1, 65536); NEED("n_routed_experts", experts, 1, 4096);
    NEED("num_experts_per_tok", topk, 1, 256);
    OPT("unpadded_vocab_size", unpadded, m->vocab, 1, m->vocab);
    OPT("swa_num_attention_heads", sh, heads, 1, 65536); OPT("swa_num_key_value_heads", sk, 16, 1, 65536);
    OPT("swa_head_dim", sd, hd, 1, 65536); OPT("sliding_window_size", window, 512, 1, 1048576);
    OPT("d_rel", rel, 16, 1, 65536); OPT("rel_extent", extent, 1024, 1, 1048576);
    OPT("conv_kernel_size", conv, 4, 2, 65536); OPT("sconv_kernel_size", conv, conv, 2, 65536);
    OPT("n_shared_experts", shared, 2, 1, 65536); OPT("dense_mlp_idx", first, 0, 0, layers);
    if (lmb_json_member(json, "dense_intermediate_size")) {
        NEED("dense_intermediate_size", dense, 1, 1048576);
        NEED("intermediate_size", inter, 1, 1048576);
    } else {
        NEED("intermediate_size", dense, 1, 1048576);
        NEED("moe_intermediate_size", inter, 1, 1048576);
    }
#undef NEED
#undef OPT
    if (!m->vocab || m->vocab > INT32_MAX || !unpadded || h != m->hidden ||
        layers != m->layers || heads % kv || sh % sk || topk > experts) return -1;
    for (uint32_t i = 0; i < layers; i++) { v.local[i] = (i + 1) % 6 != 0; v.sparse[i] = i >= first; }
    if (lmb_json_member(json, "layer_types")) {
        if (lmb_plan_kinds(json, "layer_types", "hybrid", "hybrid_sliding", v.local, layers)) return -1;
    } else if (lmb_json_member(json, "local_layer_ids")) {
        uint64_t ids[256]; unsigned count;
        if (lmb_plan_array(lmb_json_member(json, "local_layer_ids"), ids, 256, &count)) return -1;
        memset(v.local, 0, sizeof v.local);
        for (unsigned i = 0; i < count; i++) { if (ids[i] >= layers) return -1; v.local[ids[i]] = 1; }
    }
    if (lmb_json_member(json, "mlp_layer_types") &&
        lmb_plan_kinds(json, "mlp_layer_types", "dense", "sparse", v.sparse, layers)) return -1;
    v.hidden = h; v.inter = inter; v.experts = experts;
    if (lmb_plan_tensors(root, lmb_inkling_tensor, &v) || (v.edge_seen & 7) != 7) return -1;
    int first_sparse = -1;
    for (uint32_t i = 0; i < layers; i++) {
        uint32_t required = (1u << 14) - 1;
        required |= v.sparse[i] ? ((1u << 24) - (1u << 17)) : 7u << 14;
        if (v.packed[i]) {
            if (v.packed[i] != 3) return -1;
            required |= 3u << 24;
        }
        if ((v.seen[i] & required) != required) return -1;
        if (v.sparse[i]) {
            if (first_sparse >= 0 && memcmp(v.encoding[i], v.encoding[first_sparse], 2)) return -1;
            first_sparse = (int)i;
        }
        uint64_t kvdim = (uint64_t)(v.local[i] ? sk : kv) * (v.local[i] ? sd : hd);
        m->memory[i].state_token_bytes = kvdim * 8;
        m->memory[i].state_context_limit = v.local[i] ? window : 0;
        m->memory[i].state_fixed_bytes = lmb_size_mul(2 * (kvdim + h), (uint64_t)(conv - 1) * 4);
        m->memory[i].resident_bytes = lmb_size_add(m->memory[i].resident_bytes, 32768);
    }
    char path[1024]; struct stat st;
    int len = snprintf(path, sizeof path, "%s/dense-int4g64", root);
    if (len < 0 || (size_t)len >= sizeof path) return -1;
    if (!stat(path, &st)) {
        v.sidecar = 1;
        if (!S_ISDIR(st.st_mode) || lmb_plan_tensors(path, lmb_inkling_tensor, &v)) return -1;
    } else if (errno != ENOENT) return -1;
    uint64_t wide = h + (uint64_t)(heads + 2u * kv) * hd + (uint64_t)(sh + 2u * sk) * sd +
        (uint64_t)(heads + sh) * rel + (uint64_t)shared * (h + inter) +
        (uint64_t)topk * (h + inter) + experts + dense;
    m->scratch_fixed_bytes = lmb_size_add(lmb_size_mul(wide, 128u * 16u * 4u),
        lmb_size_add((uint64_t)h * inter * 24u, (uint64_t)(extent + window + conv + 128u) * 256u * 4u));
    m->scratch_token_bytes = 256u * 4u;
    m->segment_fixed_bytes = (uint64_t)layers * (4096u + (uint64_t)experts * 256u);
    m->max_context = 1048576; /* household admission ceiling */
    m->experts = experts; m->experts_per_tok = topk; m->moe_intermediate = inter;
    m->memory_contract = 1;
    return 0;
}
#endif
