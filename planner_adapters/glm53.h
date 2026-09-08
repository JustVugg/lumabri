#ifndef LUMABRI_PLAN_GLM53_H
#define LUMABRI_PLAN_GLM53_H
/* Initial float-source text contract. Packed containers and the vision tower
 * are deliberately not inferred from this contract. Colibri remains unchanged. */
typedef struct {
    LmbModelShape *model;
    uint32_t h, heads, qrank, kvrank, nope, vd, inter, dense, experts, shared;
    uint32_t first, kh, kd, conv, ih, id, pool, hc;
    uint8_t full[120], prefix, edge_seen, *experts_seen;
    uint64_t seen[120], largest;
    uint32_t low[120][4];
} LmbGlm53Inventory;

static int lmb_glm53_tensor(const LmbPlanTensor *t, void *opaque) {
    LmbGlm53Inventory *v = opaque;
    LmbModelShape *m = v->model;
    const char *name = t->name;
    if (!strncmp(name, "model.visual.", 13)) return -1;
    if (strcmp(t->dtype, "F32") && strcmp(t->dtype, "BF16") && strcmp(t->dtype, "F16")) return -1;
    if (strncmp(name, "lm_head.weight", 14)) {
        uint8_t prefix = !strncmp(name, "model.language_model.", 21) ? 2 :
                         !strncmp(name, "model.", 6) ? 1 : 0;
        if (!prefix) return 0;
        if (v->prefix && v->prefix != prefix) return -1;
        v->prefix = prefix;
        name += prefix == 2 ? 21 : 6;
    }
    int global = !strcmp(name, "embed_tokens.weight") ? 0 :
        !strcmp(name, "norm.weight") ? 1 : !strcmp(name, "lm_head.weight") ? 2 : -1;
    if (global >= 0) {
        uint64_t count = global == 1 ? m->hidden : (uint64_t)m->vocab * m->hidden;
        if ((v->edge_seen & (1u << global)) || t->elements != count ||
            (global != 1 && (t->rank != 2 || t->shape[1] != m->hidden))) return -1;
        v->edge_seen |= 1u << global;
        m->edge_resident_bytes = lmb_size_add(m->edge_resident_bytes, lmb_size_mul(count, 8));
        if (count * 4 > m->edge_scratch_fixed_bytes) m->edge_scratch_fixed_bytes = count * 4;
        return m->edge_resident_bytes == UINT64_MAX ? -1 : 0;
    }
    if (strncmp(name, "layers.", 7)) return 0;
    uint64_t layer;
    const char *field = lmb_plan_uint(name + 7, &layer);
    if (!field || *field++ != '.') return -1;
    if (layer >= m->layers) return 0;
    uint64_t H = v->h, P = (uint64_t)v->kh * v->kd, HC = v->hc;
    uint64_t hyper = HC * HC + 2 * HC;
    static const char *const names[] = {
        "input_layernorm.weight", "post_attention_layernorm.weight",
        "hc_attn_fn", "hc_attn_base", "hc_attn_scale", "hc_ffn_fn", "hc_ffn_base", "hc_ffn_scale",
        "self_attn.q_a_proj.weight", "self_attn.q_a_layernorm.weight", "self_attn.q_b_proj.weight",
        "self_attn.kv_a_proj_with_mqa.weight", "self_attn.kv_a_layernorm.weight", "self_attn.kv_b_proj.weight",
        "self_attn.o_proj.weight", "self_attn.indexer.wq_b.weight", "self_attn.indexer.wk.weight",
        "self_attn.indexer.weights_proj.weight", "self_attn.indexer.k_norm.weight", "self_attn.indexer.k_norm.bias",
        "self_attn.indexer.index_kpool_compress_ape", "self_attn.indexer.index_kpool_compress_gate",
        "self_attn.q_proj.weight", "self_attn.k_proj.weight", "self_attn.v_proj.weight",
        "self_attn.g_a_proj.weight", "self_attn.g_b_proj.weight", "self_attn.f_a_proj.weight", "self_attn.f_b_proj.weight",
        "self_attn.b_proj.weight", "self_attn.dt_bias", "self_attn.A_log", "self_attn.o_norm.weight",
        "self_attn.q_conv1d.weight", "self_attn.k_conv1d.weight", "self_attn.v_conv1d.weight",
        "mlp.gate_proj.weight", "mlp.up_proj.weight", "mlp.down_proj.weight",
        "mlp.gate.weight", "mlp.gate.e_score_correction_bias",
        "mlp.shared_experts.gate_proj.weight", "mlp.shared_experts.up_proj.weight", "mlp.shared_experts.down_proj.weight"
    };
    uint64_t counts[] = {
        H,H, hyper*HC*H,hyper,3,hyper*HC*H,hyper,3,
        (uint64_t)v->qrank*H,v->qrank,(uint64_t)v->heads*v->nope*v->qrank,
        (uint64_t)v->kvrank*H,v->kvrank,(uint64_t)v->heads*(v->nope+v->vd)*v->kvrank,
        H*(v->full[layer] ? (uint64_t)v->heads*v->vd : P),
        (uint64_t)v->ih*v->id*v->qrank,(uint64_t)v->id*H,(uint64_t)v->ih*H,v->id,v->id,
        (uint64_t)v->pool*v->id,(uint64_t)v->id*H,
        P*H,P*H,P*H,0,0,0,0,(uint64_t)v->kh*H,P,v->kh,v->kd,
        P*v->conv,P*v->conv,P*v->conv,
        (uint64_t)v->dense*H,(uint64_t)v->dense*H,(uint64_t)v->dense*H,
        (uint64_t)v->experts*H,v->experts,
        (uint64_t)v->shared*v->inter*H,(uint64_t)v->shared*v->inter*H,(uint64_t)v->shared*v->inter*H
    };
    if (!strncmp(field, "mlp.experts.", 12)) {
        uint64_t expert;
        const char *suffix = lmb_plan_uint(field + 12, &expert);
        if (!suffix || *suffix++ != '.' || expert >= v->experts || layer < v->first) return -1;
        unsigned k = !strcmp(suffix,"gate_proj.weight") ? 0 : !strcmp(suffix,"up_proj.weight") ? 1 :
                     !strcmp(suffix,"down_proj.weight") ? 2 : 3;
        uint8_t *seen = &v->experts_seen[layer * v->experts + expert];
        if (k == 3 || (*seen & (1u << k)) || t->elements != H * v->inter ||
            t->rank != 2 || t->shape[1] != (k == 2 ? v->inter : H)) return -1;
        *seen |= 1u << k;
    } else {
        unsigned i;
        for (i=0; i<sizeof names / sizeof *names; i++) if (!strcmp(field,names[i])) break;
        if (i==sizeof names / sizeof *names) return 0;
        if (v->seen[layer] & (UINT64_C(1)<<i)) return -1;
        v->seen[layer] |= UINT64_C(1)<<i;
        if (i >=25 && i<=28) {
            /* Native low-rank work buffers have hidden-size capacity. */
            if (t->rank != 2) return -1;
            uint64_t low = (i==25 || i==27) ? t->shape[0] : t->shape[1];
            if (!low || low > H || t->elements != low * ((i==25 || i==27) ? H : P)) return -1;
            v->low[layer][i-25] = (uint32_t)low;
        } else {
            if (t->elements != counts[i]) return -1;
            uint64_t columns = 0;
            if (i==8 || i==11 || i==16 || i==17 || i==21 || (i>=22 && i<=24) ||
                i==29 || i==36 || i==37 || i==39 || i==41 || i==42) columns=H;
            else if (i==10 || i==15) columns=v->qrank;
            else if (i==13) columns=v->kvrank;
            else if (i==14) columns=v->full[layer] ? (uint64_t)v->heads*v->vd : P;
            else if (i==38) columns=v->dense;
            else if (i==43) columns=(uint64_t)v->shared*v->inter;
            if (columns && (t->rank!=2 || t->shape[1]!=columns)) return -1;
        }
    }
    /* f32 + per-row scale upper bound also covers int8 fallback for tiny
     * contraction dimensions; do not estimate packed source by file size. */
    uint64_t bytes = lmb_size_mul(t->elements, 8);
    m->memory[layer].resident_bytes = lmb_size_add(m->memory[layer].resident_bytes, bytes);
    if (bytes > v->largest) v->largest = bytes;
    return bytes == UINT64_MAX || m->memory[layer].resident_bytes == UINT64_MAX ? -1 : 0;
}

static int LMB_UNUSED lmb_glm53_memory(const char *root, const char *whole,
                                       const char *json, LmbModelShape *m) {
    if (lmb_json_member(whole, "vision_config")) return -1;
    LmbGlm53Inventory v = { .model=m };
    uint32_t layers, topk, rope, index_topk;
#define NEED(key,dst,lo,hi) if (lmb_plan_u32(json,key,lo,hi,&dst)) return -1
    NEED("hidden_size",v.h,1,65536); NEED("num_hidden_layers",layers,1,120);
    NEED("num_attention_heads",v.heads,1,1024); NEED("q_lora_rank",v.qrank,1,1048576);
    NEED("kv_lora_rank",v.kvrank,1,4096); NEED("qk_nope_head_dim",v.nope,1,65536);
    NEED("v_head_dim",v.vd,1,65536); NEED("intermediate_size",v.dense,1,16777216);
    NEED("moe_intermediate_size",v.inter,32,1048576); NEED("n_routed_experts",v.experts,1,4096);
    NEED("num_experts_per_tok",topk,1,64);
    NEED("index_topk",index_topk,1,1048576);
#undef NEED
    if (v.h!=m->hidden || layers!=m->layers || !m->vocab || m->vocab > (1u<<22) || v.inter%32 || topk>v.experts ||
        lmb_plan_optional(json,"qk_rope_head_dim",0,0,0,&rope) ||
        lmb_plan_optional(json,"n_shared_experts",1,1,64,&v.shared) ||
        lmb_plan_optional(json,"hc_mult",1,1,8,&v.hc) ||
        lmb_plan_optional(json,"index_head_dim",0,1,65536,&v.id) ||
        lmb_plan_optional(json,"index_n_heads",0,1,65536,&v.ih) ||
        lmb_plan_optional(json,"index_kpool",1,2,64,&v.pool)) return -1;
    /* The pinned MLA path calls the k-pool gate unconditionally; the loader
     * only loads it for pool > 1. Do not admit that incomplete native path. */
    if (v.pool < 2 || !v.id || !v.ih ||
        (uint64_t)v.shared*v.inter > (v.dense > v.inter ? v.dense : v.inter)) return -1;
    const char *linear = lmb_json_member(json,"linear_attn_config");
    if (!linear || *linear!='{' || lmb_plan_u32(linear,"num_heads",1,1048576,&v.kh) ||
        lmb_plan_u32(linear,"head_dim",1,512,&v.kd) ||
        lmb_plan_u32(linear,"short_conv_kernel_size",1,8,&v.conv) ||
        (uint64_t)v.kh*v.kd > 1048576) return -1;
    if (lmb_plan_kinds(json,"layer_types","linear_attention","deepseek_sparse_attention",v.full,layers)) return -1;
    uint8_t sparse[120];
    if (lmb_json_member(json,"first_k_dense_replace")) {
        if (lmb_plan_u32(json,"first_k_dense_replace",0,layers,&v.first)) return -1;
    } else {
        if (lmb_plan_kinds(json,"mlp_layer_types","dense","sparse",sparse,layers)) return -1;
        v.first=layers;
        for (uint32_t i=0;i<layers;i++) if (sparse[i]) {v.first=i; break;}
        for (uint32_t i=v.first;i<layers;i++) if (!sparse[i]) return -1;
    }
    /* The optional alternate layout must agree with the native loader. */
    if (lmb_json_member(linear,"full_attn_layers")) {
        uint8_t alternate[120]={0};
        if (lmb_plan_indices(linear,"full_attn_layers",alternate,layers,0) ||
            memcmp(alternate,v.full,layers)) return -1;
    }
    v.experts_seen=calloc((size_t)layers*v.experts,1);
    if (!v.experts_seen) return -1;
    int rc=lmb_plan_tensors(root,lmb_glm53_tensor,&v);
    if ((v.edge_seen & 3)!=3) rc=-1; /* missing head means tied embedding */
    for (uint32_t i=0;i<layers && !rc;i++) {
        uint64_t required=255u | (UINT64_C(1)<<14);
        if (v.full[i]) required |= ((UINT64_C(1)<<20)-(UINT64_C(1)<<8)) |
            (v.pool>1 ? (UINT64_C(3)<<20) : 0);
        else required |= (UINT64_C(1)<<36)-(UINT64_C(1)<<22);
        required |= i<v.first ? (UINT64_C(7)<<36) : (UINT64_C(1)<<39)|(UINT64_C(7)<<41);
        if ((v.seen[i]&required)!=required) {rc=-1;break;}
        if (!v.full[i] && (v.low[i][0]!=v.low[i][1] || v.low[i][2]!=v.low[i][3])) {rc=-1;break;}
        if (i>=v.first) for (uint32_t e=0;e<v.experts;e++) if (v.experts_seen[i*v.experts+e]!=7) rc=-1;
        /* Pinned session_open allocates EVERY layer's state on EVERY range. */
        if (v.full[i]) m->session_token_bytes=lmb_size_add(m->session_token_bytes,(uint64_t)(v.kvrank+2*v.id)*4);
        else m->session_fixed_bytes=lmb_size_add(m->session_fixed_bytes,
            ((uint64_t)v.kh*v.kd*v.kd+3u*(uint64_t)v.kh*v.kd*v.conv)*4);
    }
    free(v.experts_seen);
    if (rc) return -1;
    m->session_fixed_bytes=lmb_size_add(m->session_fixed_bytes,
        ((uint64_t)3*v.kh*v.kd+v.kd)*4 + (uint64_t)layers*1024);
    m->boundary_width=v.h*v.hc;
    uint64_t wide=m->boundary_width+(uint64_t)v.heads*(v.nope+v.kvrank+v.vd)+
        (uint64_t)v.ih*v.id+v.qrank+v.kvrank+v.h+(uint64_t)v.kh*v.kd+v.dense+
        (uint64_t)v.shared*v.inter+v.experts+topk+(uint64_t)index_topk*v.pool+v.pool;
    /* This adapter permits context-sized prefill requests, unlike the
     * 128-row adapters. Bound buffers by context, not by a shared assumption. */
    m->scratch_token_bytes=lmb_size_mul(wide,64);
    m->scratch_fixed_bytes=lmb_size_add(v.largest,lmb_size_mul(wide,64));
    m->segment_fixed_bytes=(uint64_t)layers*(8192u+(uint64_t)v.experts*512);
    m->max_context=1048576;
    m->memory_contract=1;
    return 0;
}
#endif
