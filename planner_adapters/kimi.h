#ifndef LUMABRI_PLAN_KIMI_H
#define LUMABRI_PLAN_KIMI_H

/* Float/BF16 dense source with native MXFP4 routed experts. Prepared dense
 * quantization needs its own retained-layout validation before admission. */
static const char *const lmb_kimi_fields[] = {
    "input_layernorm.weight", "post_attention_layernorm.weight",
    "self_attention_res_norm.weight", "self_attention_res_proj.weight",
    "mlp_res_norm.weight", "mlp_res_proj.weight",
    "self_attn.q_proj.weight", "self_attn.k_proj.weight", "self_attn.v_proj.weight",
    "self_attn.g_proj.weight", "self_attn.o_proj.weight",
    "self_attn.q_conv1d.weight", "self_attn.k_conv1d.weight", "self_attn.v_conv1d.weight",
    "self_attn.f_a_proj.weight", "self_attn.f_b_proj.weight", "self_attn.b_proj.weight",
    "self_attn.dt_bias", "self_attn.o_norm.weight", "self_attn.A_log",
    "self_attn.q_a_proj.weight", "self_attn.q_b_proj.weight",
    "self_attn.kv_a_proj_with_mqa.weight", "self_attn.kv_b_proj.weight",
    "self_attn.q_a_layernorm.weight", "self_attn.kv_a_layernorm.weight",
    "self_attn.w_k.weight", "self_attn.w_q.weight", "self_attn.w_p.weight",
    "self_attn.kn_w", "self_attn.kn_b",
    "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight",
    "block_sparse_moe.gate.weight", "block_sparse_moe.gate.e_score_correction_bias",
    "block_sparse_moe.routed_expert_norm.weight",
    "block_sparse_moe.routed_expert_down_proj.weight", "block_sparse_moe.routed_expert_up_proj.weight",
    "block_sparse_moe.shared_experts.gate_proj.weight",
    "block_sparse_moe.shared_experts.up_proj.weight", "block_sparse_moe.shared_experts.down_proj.weight"
};
typedef struct {
    LmbModelShape *model;
    uint64_t seen[128];
    uint8_t kda[128], index[128], edge_seen, prefix_seen;
    uint8_t *expert_seen;
    uint32_t first, latent, inter, experts;
} LmbKimiInventory;

static int lmb_kimi_tensor(const LmbPlanTensor *t, void *opaque) {
    LmbKimiInventory *v = opaque;
    LmbModelShape *m = v->model;
    const char *name = t->name;
    unsigned prefix = 1;
    if (!strncmp(name, "language_model.", 15)) { name += 15; prefix = 2; }
    int global = !strcmp(name, "model.embed_tokens.weight") ? 0 :
        !strcmp(name, "lm_head.weight") ? 1 : !strcmp(name, "model.norm.weight") ? 2 :
        !strcmp(name, "model.output_attn_res_norm.weight") ? 3 :
        !strcmp(name, "model.output_attn_res_proj.weight") ? 4 : -1;
    int is_float = !strcmp(t->dtype, "F32") || !strcmp(t->dtype, "BF16") || !strcmp(t->dtype, "F16");
    if (global >= 0) {
        v->prefix_seen |= prefix;
        if (!is_float || v->prefix_seen == 3 || (v->edge_seen & (1u << global)) ||
            t->elements != (global < 2 ? (uint64_t)m->vocab * m->hidden : m->hidden)) return -1;
        v->edge_seen |= 1u << global;
        /* Embeddings are read per row; reserve their complete source footprint
         * as well as the f32 head upper bound, not a fictitious small Edge. */
        m->edge_resident_bytes = lmb_size_add(m->edge_resident_bytes, lmb_size_mul(t->elements, 4));
        return m->edge_resident_bytes == UINT64_MAX ? -1 : 0;
    }
    if (strncmp(name, "model.layers.", 13)) return 0;
    uint64_t layer;
    const char *field = lmb_plan_uint(name + 13, &layer);
    if (!field || *field++ != '.') return -1;
    if (layer >= m->layers) return 0;
    v->prefix_seen |= prefix;
    if (v->prefix_seen == 3) return -1;
    uint64_t cost;
    const char *ep = "block_sparse_moe.experts.";
    if (!strncmp(field, ep, strlen(ep))) {
        uint64_t expert;
        const char *kind = lmb_plan_uint(field + strlen(ep), &expert);
        if (!kind || *kind++ != '.' || expert >= v->experts || layer < v->first) return -1;
        const char *const names[] = {"w1.weight_packed", "w1.weight_scale",
            "w2.weight_packed", "w2.weight_scale", "w3.weight_packed", "w3.weight_scale"};
        unsigned i;
        for (i = 0; i < 6; i++) if (!strcmp(kind, names[i])) break;
        if (i == 6) return -1;
        uint64_t expected = (uint64_t)v->latent * v->inter / (i % 2 ? 32u : 2u);
        uint8_t *seen = &v->expert_seen[layer * v->experts + expert];
        if ((*seen & (1u << i)) || strcmp(t->dtype, "U8") || t->bytes != expected) return -1;
        *seen |= 1u << i;
        cost = t->bytes; /* native MXFP4 packed nibbles and e8m0 scales */
    } else {
        unsigned i;
        for (i = 0; i < sizeof lmb_kimi_fields / sizeof *lmb_kimi_fields; i++)
            if (!strcmp(field, lmb_kimi_fields[i])) break;
        if (i == sizeof lmb_kimi_fields / sizeof *lmb_kimi_fields) return 0;
        if (!is_float || (v->seen[layer] & (UINT64_C(1) << i))) return -1;
        v->seen[layer] |= UINT64_C(1) << i;
        /* Upper bound for all supported dense numeric profiles: f32 plus row
         * scales. Never assume that a donor inherited the source's K3_BITS. */
        cost = lmb_size_add(lmb_size_mul(t->elements, 4), lmb_size_mul(t->shape[0], 4));
    }
    m->memory[layer].resident_bytes = lmb_size_add(m->memory[layer].resident_bytes, cost);
    return m->memory[layer].resident_bytes == UINT64_MAX ? -1 : 0;
}

static int lmb_plan_indices(const char *json, const char *key, uint8_t *dst,
                             uint32_t layers, unsigned origin) {
    uint64_t ids[128]; unsigned count;
    if (lmb_plan_array(lmb_json_member(json, key), ids, 128, &count)) return -1;
    memset(dst, 0, layers);
    for (unsigned i = 0; i < count; i++) {
        if (ids[i] < origin || ids[i] - origin >= layers) return -1;
        dst[ids[i] - origin] = 1;
    }
    return 0;
}

static int LMB_UNUSED lmb_kimi_memory(const char *root, const char *json, LmbModelShape *m) {
    LmbKimiInventory v = { .model = m };
    uint32_t h, layers, heads, qrank, kvrank, nope, rope, vd, experts, topk, inter, latent;
    uint32_t dense, shared, first, block, kh, kd, conv, index, index_heads, index_topk;
#define NEED(key, dst, lo, hi) if (lmb_plan_u32(json, key, lo, hi, &dst)) return -1
    NEED("hidden_size", h, 1, 65536); NEED("num_hidden_layers", layers, 1, 128);
    NEED("num_attention_heads", heads, 1, 65536); NEED("q_lora_rank", qrank, 1, 65536);
    NEED("kv_lora_rank", kvrank, 1, 4096); NEED("qk_nope_head_dim", nope, 1, 65536);
    NEED("qk_rope_head_dim", rope, 2, 256); NEED("v_head_dim", vd, 1, 65536);
    NEED("num_experts", experts, 1, 4096); NEED("num_experts_per_token", topk, 1, 64);
    NEED("moe_intermediate_size", inter, 32, 1048576); NEED("routed_expert_hidden_size", latent, 32, 1048576);
    NEED("intermediate_size", dense, 1, 1048576); NEED("num_shared_experts", shared, 1, 65536);
    NEED("first_k_dense_replace", first, 0, layers); NEED("attn_res_block_size", block, 1, layers);
#undef NEED
    const char *linear = lmb_json_member(json, "linear_attn_config");
    if (!linear || *linear != '{' || lmb_plan_u32(linear, "num_heads", 1, 65536, &kh) ||
        lmb_plan_u32(linear, "head_dim", 1, 512, &kd) ||
        lmb_plan_u32(linear, "short_conv_kernel_size", 1, 8, &conv) ||
        lmb_plan_indices(linear, "kda_layers", v.kda, layers, 1) ||
        lmb_plan_optional(json, "index_hd", 0, 0, 65536, &index) ||
        lmb_plan_optional(json, "index_nh", 0, 0, 65536, &index_heads) ||
        lmb_plan_optional(json, "index_topk", 0, 0, 1048576, &index_topk)) return -1;
    uint32_t blocks = (layers + block - 1) / block;
    uint64_t proj = (uint64_t)kh * kd;
    if (h != m->hidden || layers != m->layers || !m->vocab || m->vocab > (1u << 22) ||
        topk > experts || latent % 32 || inter % 32 || rope % 2 || blocks + 1 > 16 ||
        proj > (1u << 20) || (index && (!index_heads || !index_topk))) return -1;
    for (uint32_t i = 0; i < layers; i++) v.index[i] = index && !v.kda[i];
    if (index && lmb_json_member(json, "index_layers") &&
        lmb_plan_indices(json, "index_layers", v.index, layers, 1)) return -1;
    v.first = first; v.latent = latent; v.inter = inter; v.experts = experts;
    v.expert_seen = calloc((size_t)layers * experts, 1);
    if (!v.expert_seen) return -1;
    int rc = lmb_plan_tensors(root, lmb_kimi_tensor, &v);
    if (v.edge_seen != 31) rc = -1;
    for (uint32_t i = 0; i < layers && !rc; i++) {
        uint64_t required = 63; /* norms and AttnRes scores */
        required |= v.kda[i] ? ((UINT64_C(1) << 20) - (UINT64_C(1) << 6)) :
            ((UINT64_C(1) << 26) - (UINT64_C(1) << 20)) | (3u << 9);
        if (!v.kda[i] && v.index[i]) required |= (UINT64_C(31) << 26);
        required |= i < first ? (UINT64_C(7) << 31) : (UINT64_C(255) << 34);
        if ((v.seen[i] & required) != required) { rc = -1; break; }
        if (i >= first) {
            for (uint32_t e = 0; e < experts; e++)
                if (v.expert_seen[i * experts + e] != 63) rc = -1;
            /* Every native slot allocates e_slot + 8192 for aligned O_DIRECT
             * leading/trailing padding, even on its buffered-read fallback. */
            m->memory[i].resident_bytes = lmb_size_add(m->memory[i].resident_bytes,
                                                       (uint64_t)experts * 8192u);
        }
        if (v.kda[i]) m->memory[i].state_fixed_bytes = (proj * kd + 3u * proj * conv) * 4;
        else m->memory[i].state_token_bytes = (kvrank + rope + (v.index[i] ? index : 0u)) * 4u;
    }
    free(v.expert_seen);
    if (rc) return -1;
    m->boundary_width = h * (1u + blocks);
    m->edge_scratch_fixed_bytes = (uint64_t)h * 1024u * 4u; /* native head load quantization */
    uint64_t wide = m->boundary_width + (uint64_t)heads * (nope + rope + 2u * vd) +
        proj * (kd + 8u) + (uint64_t)shared * inter + dense + latent + qrank + kvrank +
        (uint64_t)index * (index_heads + 1u) + experts + (uint64_t)topk * (latent + inter);
    m->scratch_fixed_bytes = lmb_size_add(lmb_size_mul(wide, 128u * 16u * 4u),
        lmb_size_mul(wide, 1024u * 4u)); /* conservative QCHUNK loading workspace */
    m->scratch_token_bytes = 256u * 32u; /* score + DSA entries and selections per team */
    m->segment_fixed_bytes = (uint64_t)layers * (4096u + (uint64_t)experts * 512u);
    m->max_context = 1048576;
    m->experts = experts; m->experts_per_tok = topk; m->moe_intermediate = inter;
    m->memory_contract = 1;
    return 0;
}
#endif
