/* Resident CPU contract for Colibri qwen36's converted container.
 * Derived from load_meta/model_init_range, Segment session_create and Edge
 * engine_open at upstream 12a5c464. No runtime source is modified. */
#ifndef LUMABRI_PLAN_QWEN36_H
#define LUMABRI_PLAN_QWEN36_H
#include <errno.h>
#include "tensors.h"

typedef struct {
    LmbModelShape *m;
    uint64_t counts[23], weight_bytes, scale_elements;
    uint32_t seen[LMB_PLAN_LAYER_MAX];
    uint8_t full[LMB_PLAN_LAYER_MAX], edge, *experts;
} LmbQwen36Inventory;

static int lmb_q36_tensor(const LmbPlanTensor *t, void *opaque) {
    LmbQwen36Inventory *v=opaque; LmbModelShape *m=v->m;
    unsigned floating=!strcmp(t->dtype,"F32") || !strcmp(t->dtype,"F16") || !strcmp(t->dtype,"BF16");
    int edge=!strcmp(t->name,"model.embed_tokens.weight") ? 0 :
        !strcmp(t->name,"lm_head.weight") ? 1 : !strcmp(t->name,"model.norm.weight") ? 2 : -1;
    if(edge>=0) {
        uint64_t count=edge<2 ? (uint64_t)m->vocab*m->hidden : m->hidden;
        if(!floating || t->elements!=count || (v->edge&(1u<<edge))) return -1;
        v->edge|=1u<<edge; return 0;
    }
    if(strncmp(t->name,"model.layers.",13)) return 0;
    uint64_t layer;
    const char *field=lmb_plan_uint(t->name+13,&layer);
    if(!field || *field++!='.' || layer>=m->layers) return -1;
    if(!strncmp(field,"mlp.experts.",12)) {
        uint64_t expert;
        const char *kind=lmb_plan_uint(field+12,&expert);
        if(!kind || *kind++!='.' || expert>=m->experts) return -1;
        unsigned bit;
        if(!strcmp(kind,"merged_weight")) {
            if((strcmp(t->dtype,"I8") && strcmp(t->dtype,"U8")) ||
               (t->bytes!=v->weight_bytes &&
                ((v->weight_bytes&1) || t->bytes!=v->weight_bytes/2))) return -1;
            bit=1;
        } else if(!strcmp(kind,"qs")) {
            if(!floating || t->elements!=v->scale_elements) return -1;
            bit=2;
        } else return -1;
        uint8_t *seen=&v->experts[layer*m->experts+expert];
        if(*seen&bit) return -1;
        *seen|=bit; return 0;
    }
    static const char *const names[]={"input_layernorm.weight","post_attention_layernorm.weight",
        "mlp.gate.weight","mlp.shared_expert.gate_proj.weight","mlp.shared_expert.up_proj.weight",
        "mlp.shared_expert.down_proj.weight","mlp.shared_expert_gate.weight","self_attn.q_norm.weight",
        "self_attn.k_norm.weight","mlp.gate.e_score_correction_bias",
        "self_attn.q_proj.weight","self_attn.k_proj.weight","self_attn.v_proj.weight","self_attn.o_proj.weight",
        "linear_attn.in_proj_qkv.weight","linear_attn.in_proj_z.weight","linear_attn.in_proj_b.weight",
        "linear_attn.in_proj_a.weight","linear_attn.conv1d.weight","linear_attn.dt_bias","linear_attn.A_log",
        "linear_attn.norm.weight","linear_attn.out_proj.weight"};
    unsigned i;
    for(i=0;i<23;i++) if(!strcmp(field,names[i])) break;
    if(i==23) return 0;
    if(!floating || t->elements!=v->counts[i] || (v->seen[layer]&(1u<<i))) return -1;
    v->seen[layer]|=1u<<i; return 0;
}

static int LMB_UNUSED lmb_plan_u32(const char *json, const char *key,
                                  uint32_t minimum, uint32_t maximum, uint32_t *out) {
    const char *p = lmb_json_member(json, key);
    if (!p || *p < '0' || *p > '9') return -1;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return -1;
    errno = 0; char *end;
    unsigned long long value = strtoull(p, &end, 10);
    if (errno || value < minimum || value > maximum) return -1;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end != ',' && *end != '}' && *end != ']') return -1;
    *out = (uint32_t)value;
    return 0;
}

static int LMB_UNUSED lmb_qwen36_metadata(const char *root, LmbModelShape *m, LmbQwen36Inventory *v) {
    char path[1024], json[65536];
    int length = snprintf(path, sizeof path, "%s/qwen36_meta.json", root);
    if (length < 0 || (size_t)length >= sizeof path) return -1;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    size_t n = fread(json, 1, sizeof json - 1, file);
    int invalid = ferror(file) || !feof(file);
    fclose(file); json[n] = 0;
    if (invalid || !n || memchr(json,0,n) || *lmb_plan_space(json)!='{') return -1;
    const char *finish=lmb_plan_object_end(lmb_plan_space(json));
    if(!finish || *lmb_plan_space(finish)) return -1;
    uint32_t hidden, layers, experts, topk, inter, shared, heads, kv_heads, hd;
    uint32_t qd, kd, vd, oi, gs, vh, kh, kdim, vdim, convk, convdim;
#define NEED(key, field, lo, hi) if (lmb_plan_u32(json, key, lo, hi, &field)) return -1
    NEED("hidden", hidden, 1, 65536); NEED("n_layers", layers, 1, LMB_PLAN_LAYER_MAX);
    NEED("num_experts", experts, 1, 1024); NEED("topk", topk, 1, 256);
    NEED("moe_inter", inter, 1, 65536); NEED("shared_inter", shared, 1, 65536);
    NEED("q_heads", heads, 1, 65536); NEED("kv_heads", kv_heads, 1, 65536);
    NEED("head_dim", hd, 1, 65536); NEED("q_head_dim", qd, 1, 131072);
    NEED("k_head_dim", kd, 1, 65536); NEED("v_head_dim", vd, 1, 65536);
    NEED("o_in", oi, 1, 1048576); NEED("expert_gs", gs, 0, 65536);
    NEED("dn_vheads", vh, 1, 65536); NEED("dn_kheads", kh, 1, 65536);
    NEED("dn_kdim", kdim, 1, 65536); NEED("dn_vdim", vdim, 1, 512);
    NEED("dn_convk", convk, 2, 65536); NEED("dn_conv_dim", convdim, 1, 1048576);
#undef NEED
    if (hidden != m->hidden || layers != m->layers || !m->vocab ||
        experts < topk || heads % kv_heads || vh % kh || kd != vd || kd != hd ||
        oi != (uint64_t)heads * hd ||
        convdim != 2u * (uint64_t)kh * kdim + (uint64_t)vh * vdim) return -1;
    const char *kinds = lmb_json_member(json, "layer_types");
    if (!kinds || *kinds++ != '[') return -1;
    uint8_t full[LMB_PLAN_LAYER_MAX];
    for (uint32_t i = 0; i < layers; i++) {
        while (*kinds == ' ' || *kinds == '\n' || *kinds == '\r' || *kinds == '\t') kinds++;
        if (!strncmp(kinds, "\"full_attention\"", 16)) { full[i] = 1; kinds += 16; }
        else if (!strncmp(kinds, "\"linear_attention\"", 18)) { full[i] = 0; kinds += 18; }
        else return -1;
        while (*kinds == ' ' || *kinds == '\n' || *kinds == '\r' || *kinds == '\t') kinds++;
        if (*kinds++ != (i + 1 == layers ? ']' : ',')) return -1;
    }
    uint64_t scales = gs ? 2u * (uint64_t)inter * ((hidden + gs - 1) / gs) +
        (uint64_t)hidden * ((inter + gs - 1) / gs) : 2u * (uint64_t)inter + hidden;
    /* CPU caches keep unpacked int8 even for packed int4 source weights. */
    uint64_t slot = lmb_size_add(3u * (uint64_t)hidden * inter, scales * 4);
    uint64_t expert_bytes = lmb_size_mul(slot, experts);
    uint64_t common = 3u * (uint64_t)hidden + (uint64_t)experts * hidden + experts +
        3u * (uint64_t)hidden * shared + 2u * hd;
    uint64_t attn = (uint64_t)heads * qd * hidden +
        (uint64_t)kv_heads * (kd + vd) * hidden + (uint64_t)hidden * oi;
    uint64_t linear = (uint64_t)convdim * hidden + 2u * (uint64_t)vh * vdim * hidden +
        2u * (uint64_t)vh * hidden + (uint64_t)convdim * convk + 2u * vh + vdim;
    uint64_t recurrent = (uint64_t)vh * kdim * vdim + (uint64_t)convdim * (convk - 1);
    for (uint32_t i = 0; i < layers; i++) {
        m->memory[i].resident_bytes = lmb_size_add(expert_bytes,
            lmb_size_mul(lmb_size_add(common, full[i] ? attn : linear), 4));
        m->memory[i].state_token_bytes = full[i] ? (uint64_t)kv_heads * kd * 8 : 0;
        m->memory[i].state_fixed_bytes = full[i] ? 0 : recurrent * 4;
    }
    m->edge_resident_bytes = lmb_size_mul(lmb_size_add(
        lmb_size_mul((uint64_t)m->vocab * hidden, 2), hidden), 4);
    /* Small per-layer structures, expert indices and pilot arrays are kept
     * for all layers even by an interval engine; bound them independently. */
    m->segment_fixed_bytes = lmb_size_mul(layers, 4096u + (uint64_t)experts * 256);
    uint64_t widest = hidden + (uint64_t)heads * qd + (uint64_t)kv_heads * (kd + vd) +
        oi + convdim + (uint64_t)vh * (kdim + vdim) + inter + shared + experts;
    m->scratch_fixed_bytes = lmb_size_add(slot,
        lmb_size_add(lmb_size_mul(widest, 128u * 16u * 4u), 32u << 20));
    m->scratch_token_bytes = 256u * 4u; /* maximum admitted OpenMP team */
    m->max_context = 262144; /* QWEN36_ATTN_MAX_CTX in the pinned adapter */
    m->experts = experts; m->experts_per_tok = topk; m->moe_intermediate = inter;
    m->heads = heads; m->kv_heads = kv_heads;
    m->memory_contract = 1;
    *v=(LmbQwen36Inventory){.m=m,.weight_bytes=3u*(uint64_t)hidden*inter,.scale_elements=scales};
    memcpy(v->full,full,layers);
    uint64_t H=hidden, DV=(uint64_t)vh*vdim;
    uint64_t counts[]={H,H,(uint64_t)experts*H,H*shared,H*shared,H*shared,H,hd,hd,experts,
        (uint64_t)heads*qd*H,(uint64_t)kv_heads*kd*H,(uint64_t)kv_heads*vd*H,H*oi,
        (uint64_t)convdim*H,DV*H,(uint64_t)vh*H,(uint64_t)vh*H,(uint64_t)convdim*convk,
        vh,vh,vdim,H*DV};
    memcpy(v->counts,counts,sizeof counts);
    return 0;
}

static int LMB_UNUSED lmb_qwen36_memory(const char *root, LmbModelShape *m) {
    LmbQwen36Inventory v;
    if(lmb_qwen36_metadata(root,m,&v)) return -1;
    v.experts=calloc((size_t)m->layers*m->experts,1);
    if(!v.experts) return -1;
    int rc=lmb_plan_tensors(root,lmb_q36_tensor,&v);
    if(v.edge!=7) rc=-1;
    for(unsigned i=0;i<m->layers && !rc;i++) {
        uint32_t required=63u | (v.full[i] ? (15u<<10) : (511u<<14));
        if((v.seen[i]&required)!=required) rc=-1;
        for(unsigned e=0;e<m->experts;e++) if(v.experts[i*m->experts+e]!=3) rc=-1;
    }
    free(v.experts); return rc;
}
#endif
