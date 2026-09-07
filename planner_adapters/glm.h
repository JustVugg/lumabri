#ifndef LUMABRI_PLAN_GLM_H
#define LUMABRI_PLAN_GLM_H
/* Float-source GLM/MLA contract. Prepared quantized containers must not be
 * inferred from file bytes: their per-tensor format stamps need validation. */
typedef struct {
    LmbModelShape *model;
    uint32_t seen[128], first, inter, experts;
    uint8_t index[128], edge_seen, *expert_seen;
    uint64_t largest_dense, largest_expert;
} LmbGlmInventory;

static int lmb_glm_tensor(const LmbPlanTensor *t, void *opaque) {
    LmbGlmInventory *v = opaque;
    LmbModelShape *m = v->model;
    const char *name = t->name;
    size_t name_len = strlen(name);
    if (name_len >= 3 && !strcmp(name + name_len - 3, ".qs")) return -1;
    int global = !strcmp(name, "model.embed_tokens.weight") ? 0 :
        !strcmp(name, "lm_head.weight") ? 1 : !strcmp(name, "model.norm.weight") ? 2 : -1;
    int floating = !strcmp(t->dtype, "F32") || !strcmp(t->dtype, "BF16") || !strcmp(t->dtype, "F16");
    if (global >= 0) {
        if (!floating || (v->edge_seen & (1u << global)) || t->elements !=
            (global < 2 ? (uint64_t)m->vocab * m->hidden : m->hidden)) return -1;
        v->edge_seen |= 1u << global;
        uint64_t cost = lmb_size_mul(t->elements, 4);
        m->edge_resident_bytes = lmb_size_add(m->edge_resident_bytes,
            lmb_size_add(cost, (uint64_t)m->vocab * 4));
        if (cost > m->edge_scratch_fixed_bytes) m->edge_scratch_fixed_bytes = cost;
        return m->edge_resident_bytes == UINT64_MAX ? -1 : 0;
    }
    if (strncmp(name, "model.layers.", 13)) return 0;
    uint64_t layer;
    const char *field = lmb_plan_uint(name + 13, &layer);
    if (!field || *field++ != '.') return -1;
    if (layer >= m->layers) return 0;
    static const char *const fields[] = {
        "input_layernorm.weight", "post_attention_layernorm.weight",
        "self_attn.q_a_proj.weight", "self_attn.q_a_layernorm.weight",
        "self_attn.q_b_proj.weight", "self_attn.kv_a_proj_with_mqa.weight",
        "self_attn.kv_a_layernorm.weight", "self_attn.kv_b_proj.weight", "self_attn.o_proj.weight",
        "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight",
        "mlp.gate.weight", "mlp.gate.e_score_correction_bias",
        "mlp.shared_experts.gate_proj.weight", "mlp.shared_experts.up_proj.weight", "mlp.shared_experts.down_proj.weight",
        "self_attn.indexer.wq_b.weight", "self_attn.indexer.wk.weight",
        "self_attn.indexer.weights_proj.weight", "self_attn.indexer.k_norm.weight", "self_attn.indexer.k_norm.bias"
    };
    const char *ep = "mlp.experts.";
    int expert_tensor = !strncmp(field, ep, strlen(ep));
    if (expert_tensor) {
        uint64_t expert;
        const char *kind = lmb_plan_uint(field + strlen(ep), &expert);
        if (!kind || *kind++ != '.' || expert >= v->experts || layer < v->first) return -1;
        unsigned i = !strcmp(kind, "gate_proj.weight") ? 0 :
            !strcmp(kind, "up_proj.weight") ? 1 : !strcmp(kind, "down_proj.weight") ? 2 : 3;
        if (i == 3 || !floating || t->elements != (uint64_t)v->inter * m->hidden) return -1;
        uint8_t *seen = &v->expert_seen[layer * v->experts + expert];
        if (*seen & (1u << i)) return -1;
        *seen |= 1u << i;
    } else {
        unsigned i;
        for (i = 0; i < sizeof fields / sizeof *fields; i++) if (!strcmp(field, fields[i])) break;
        if (i == sizeof fields / sizeof *fields) return 0;
        if (!floating || (v->seen[layer] & (1u << i))) return -1;
        v->seen[layer] |= 1u << i;
    }
    uint64_t cost = lmb_size_mul(t->elements, 4);
    uint64_t *largest = expert_tensor ? &v->largest_expert : &v->largest_dense;
    if (cost > *largest) *largest = cost;
    /* f32 + a conservative scale bound also covers int3 grouped row padding
     * and all native load-time precision choices for this source encoding. */
    cost = lmb_size_add(cost, lmb_size_mul(t->elements, 1));
    m->memory[layer].resident_bytes = lmb_size_add(m->memory[layer].resident_bytes, cost);
    return m->memory[layer].resident_bytes == UINT64_MAX ? -1 : 0;
}

static int LMB_UNUSED lmb_glm_memory(const char *root, const char *json, LmbModelShape *m) {
    LmbGlmInventory v = { .model = m };
    uint32_t h, layers, heads, qrank, kvrank, nope, rope, vd, experts, topk, inter;
    uint32_t dense, shared, first, groups, index, ih, ik, freq, offset;
#define NEED(key, dst, lo, hi) if (lmb_plan_u32(json, key, lo, hi, &dst)) return -1
    NEED("hidden_size", h, 32, 1048576); NEED("num_hidden_layers", layers, 1, 128);
    NEED("num_attention_heads", heads, 1, 1024); NEED("q_lora_rank", qrank, 32, 1048576);
    NEED("kv_lora_rank", kvrank, 32, 1048576); NEED("qk_nope_head_dim", nope, 1, 65536);
    NEED("qk_rope_head_dim", rope, 2, 65536); NEED("v_head_dim", vd, 1, 65536);
    NEED("n_routed_experts", experts, 1, 4096); NEED("num_experts_per_tok", topk, 1, 64);
    NEED("moe_intermediate_size", inter, 32, 1048576); NEED("intermediate_size", dense, 32, 16777216);
    NEED("n_shared_experts", shared, 1, 64); NEED("first_k_dense_replace", first, 0, layers);
    NEED("n_group", groups, 1, 1);
#undef NEED
    if (h != m->hidden || layers != m->layers || !m->vocab || topk > experts || rope % 2 ||
        (uint64_t)heads * vd < 32 ||
        lmb_plan_optional(json, "index_head_dim", 0, 0, 256, &index) ||
        lmb_plan_optional(json, "index_n_heads", 0, 0, 65536, &ih) ||
        lmb_plan_optional(json, "index_topk", 0, 0, 1048576, &ik) ||
        lmb_plan_optional(json, "index_topk_freq", 1, 1, 128, &freq) ||
        lmb_plan_optional(json, "index_skip_topk_offset", 2, 0, 128, &offset)) return -1;
    for (uint32_t i = 0; i < layers; i++) {
        uint32_t pos = i + 1 > offset ? i + 1 - offset : 0;
        v.index[i] = pos % freq == 0;
    }
    if (lmb_json_member(json, "indexer_types") &&
        lmb_plan_kinds(json, "indexer_types", "shared", "full", v.index, layers)) return -1;
    v.first = first; v.inter = inter; v.experts = experts;
    v.expert_seen = calloc((size_t)layers * experts, 1);
    if (!v.expert_seen) return -1;
    int rc = lmb_plan_tensors(root, lmb_glm_tensor, &v);
    if (v.edge_seen != 7) rc = -1;
    int indexed = 0, missing = 0;
    if (index && ih && ik) for (uint32_t i = 0; i < layers; i++) {
        if (!v.index[i]) continue;
        uint32_t bank = v.seen[i] & (31u << 17);
        if (bank && bank != (31u << 17)) rc = -1;
        if (bank == (31u << 17)) indexed++;
        else missing++;
    }
    /* Edge detects DSA globally, while a Segment detects it for its interval.
     * A partial indexer bank would produce incompatible numeric classes. */
    if (indexed && missing) rc = -1;
    for (uint32_t i = 0; i < layers && !rc; i++) {
        uint32_t required = 511u | (i < first ? 7u << 9 : 31u << 12);
        if ((v.seen[i] & required) != required) { rc = -1; break; }
        if (i >= first) for (uint32_t e = 0; e < experts; e++)
            if (v.expert_seen[i * experts + e] != 7) rc = -1;
        m->memory[i].state_token_bytes = (uint64_t)(kvrank + rope + (indexed && v.index[i] ? index : 0)) * 4u;
    }
    free(v.expert_seen);
    if (rc) return -1;
    uint64_t wide = h + (uint64_t)heads * (nope + rope + vd) + qrank + kvrank + dense +
        (uint64_t)shared * inter + (uint64_t)topk * (h + inter) + experts + (uint64_t)ih * index;
    uint64_t loaders = experts < 256 ? experts : 256; /* up to one load per engine-team worker */
    uint64_t miss_slots = experts < 64 ? experts : 64;
    uint64_t misses = lmb_size_mul(lmb_size_mul(v.largest_expert, 15u), miss_slots);
    if (misses == UINT64_MAX) return -1;
    misses /= 4u;
    /* Native ws[64] owns additional gate/up/down slabs, separate from the
     * per-layer LRU. They persist after prefill; reserve them explicitly. */
    m->scratch_fixed_bytes = lmb_size_add(lmb_size_mul(wide, 512u * 16u * 4u),
        lmb_size_add(misses, lmb_size_add(v.largest_dense, lmb_size_mul(v.largest_expert, loaders))));
    m->scratch_token_bytes = 256u * 32u;
    m->segment_fixed_bytes = (uint64_t)(layers + 1u) * (8192u + (uint64_t)experts * 2048u);
    m->max_context = 1048576;
    m->memory_contract = 1;
    return 0;
}
#endif
