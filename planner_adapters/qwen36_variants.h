/* Converted qwen36 containers: describe the layer that actually executes.
 * CPU dense matrices expand to F32; routed weights retain their native
 * int8/int4 layout. The cap below deliberately bounds both representations.
 * GGUF has a separate contract and must not pass this inspector. */
#ifndef LUMABRI_PLAN_QWEN36_VARIANTS_H
#define LUMABRI_PLAN_QWEN36_VARIANTS_H

enum {
    Q36_IN, Q36_POST, Q36_ROUTER, Q36_SG, Q36_SU, Q36_SD, Q36_SHARED_GATE,
    Q36_QN, Q36_KN, Q36_ROUTER_BIAS, Q36_Q, Q36_K, Q36_V, Q36_O,
    Q36_DQKV, Q36_DZ, Q36_DB, Q36_DA, Q36_DCONV, Q36_DT, Q36_ALOG,
    Q36_DNORM, Q36_DOUT, Q36_G, Q36_U, Q36_D, Q36_SINKS,
    Q36_POST_OP, Q36_POST_FFN, Q36_ATTN_GATE, Q36_SC_IN, Q36_SC_CONV,
    Q36_SC_OUT, Q36_FIELDS
};
static const char *const LMB_Q36_FIELDS[Q36_FIELDS] = {
    "input_layernorm.weight", "post_attention_layernorm.weight", "mlp.gate.weight",
    "mlp.shared_expert.gate_proj.weight", "mlp.shared_expert.up_proj.weight",
    "mlp.shared_expert.down_proj.weight", "mlp.shared_expert_gate.weight",
    "self_attn.q_norm.weight", "self_attn.k_norm.weight", "mlp.gate.e_score_correction_bias",
    "self_attn.q_proj.weight", "self_attn.k_proj.weight", "self_attn.v_proj.weight", "self_attn.o_proj.weight",
    "linear_attn.in_proj_qkv.weight", "linear_attn.in_proj_z.weight", "linear_attn.in_proj_b.weight",
    "linear_attn.in_proj_a.weight", "linear_attn.conv1d.weight", "linear_attn.dt_bias", "linear_attn.A_log",
    "linear_attn.norm.weight", "linear_attn.out_proj.weight", "mlp.gate_proj.weight", "mlp.up_proj.weight",
    "mlp.down_proj.weight", "self_attn.softmax_offset", "post_operator_norm.weight",
    "post_feedforward_layernorm.weight", "self_attn.gate_proj.weight", "conv.in_proj.weight",
    "conv.conv.weight", "conv.out_proj.weight"
};
typedef struct {
    LmbModelShape *m;
    uint64_t counts[Q36_FIELDS], expected[LMB_PLAN_LAYER_MAX], seen[LMB_PLAN_LAYER_MAX];
    uint64_t expert_bytes, scales, slot;
    uint32_t dense_inter, shared_inter;
    uint8_t full[LMB_PLAN_LAYER_MAX], dense[LMB_PLAN_LAYER_MAX], edge, *experts;
    unsigned lfm3, sinks, mixed;
} LmbQ36Variants;

static int lmb_q36_bool(const char *j, const char *key, unsigned fallback, unsigned *out) {
    const char *p=lmb_json_member(j,key);
    if (!p) { *out=fallback; return 0; }
    if (!strncmp(p,"true",4)) { *out=1; p+=4; }
    else if (!strncmp(p,"false",5)) { *out=0; p+=5; }
    else return -1;
    p=lmb_plan_space(p);
    return *p==',' || *p=='}' ? 0 : -1;
}
static int lmb_q36_optional(const char *j, const char *key, uint32_t hi, uint32_t *out) {
    *out=0;
    return lmb_json_member(j,key) ? lmb_plan_u32(j,key,0,hi,out) : 0;
}
static int lmb_q36_indices(const char *j, const char *key, uint32_t layers, uint8_t *out) {
    const char *p=lmb_json_member(j,key);
    if (!p) return 0;
    if (*p++!='[') return -1;
    p=lmb_plan_space(p);
    if (*p==']') return 0;
    for (uint32_t i=0;i<layers;i++) {
        uint64_t index;
        const char *end=lmb_plan_uint(p,&index);
        if (!end || index>=layers || out[index]) return -1;
        out[index]=1; p=lmb_plan_space(end);
        if (*p==']') return 0;
        if (*p++!=',') return -1;
        p=lmb_plan_space(p);
    }
    return -1;
}
static uint64_t lmb_q36_count(const LmbQ36Variants *v, unsigned layer, unsigned field) {
    if (field>=Q36_G && field<=Q36_D)
        return (uint64_t)v->m->hidden*(v->dense[layer] ? v->dense_inter : v->shared_inter);
    return v->counts[field];
}
static int lmb_q36_variant_tensor(const LmbPlanTensor *t, void *opaque) {
    LmbQ36Variants *v=opaque; LmbModelShape *m=v->m;
    int floating=!strcmp(t->dtype,"F32") || !strcmp(t->dtype,"F16") || !strcmp(t->dtype,"BF16");
    const char *name=t->name;
    /* Upstream dense_resolve accepts this prefix on multimodal containers. */
    if (!strncmp(name,"language_model.",15)) name+=15;
    int edge=!strcmp(name,"model.embed_tokens.weight") ? 0 :
        !strcmp(name,"lm_head.weight") ? 1 : !strcmp(name,"model.norm.weight") ? 2 : -1;
    if (edge>=0) {
        uint64_t count=edge<2 ? (uint64_t)m->vocab*m->hidden : m->hidden;
        if (!floating || t->elements!=count || (v->edge&(1u<<edge))) return -1;
        v->edge|=1u<<edge; return 0;
    }
    if (strncmp(name,"model.layers.",13)) return 0;
    uint64_t layer; const char *field=lmb_plan_uint(name+13,&layer);
    if (!field || *field++!='.' || layer>=m->layers) return -1;
    if (!strncmp(field,"mlp.experts.",12)) {
        uint64_t expert; const char *kind=lmb_plan_uint(field+12,&expert);
        if (!kind || *kind++!='.' || expert>=m->experts || v->dense[layer]) return -1;
        unsigned bit;
        if (!strcmp(kind,"merged_weight")) {
            if ((strcmp(t->dtype,"I8") && strcmp(t->dtype,"U8")) ||
                (v->mixed ? t->bytes!=v->expert_bytes/3*2 :
                 (t->bytes!=v->expert_bytes && ((v->expert_bytes&1) || t->bytes!=v->expert_bytes/2)))) return -1;
            bit=1;
        } else if (!strcmp(kind,"qs")) {
            if (!floating || t->elements!=v->scales) return -1;
            bit=2;
        } else return -1;
        uint8_t *seen=&v->experts[layer*m->experts+expert];
        if (*seen&bit) return -1;
        *seen|=bit; return 0;
    }
    unsigned i;
    for (i=0;i<Q36_FIELDS;i++) if (!strcmp(field,LMB_Q36_FIELDS[i])) break;
    if (i==Q36_FIELDS) return 0;
    uint64_t count=lmb_q36_count(v,(unsigned)layer,i), bit=1ull<<i;
    if (!floating || !count || t->elements!=count || (v->seen[layer]&bit)) return -1;
    v->seen[layer]|=bit;
    /* Required weights plus optional q/k norms, router bias and shared gate.
     * Count actual metadata, never checkpoint bytes or the whole-layer mean. */
    m->memory[layer].resident_bytes=lmb_size_add(m->memory[layer].resident_bytes,lmb_size_mul(count,4));
    return 0;
}

static int lmb_q36_variants_metadata(const char *root, LmbModelShape *m, LmbQ36Variants *v) {
    char path[1024],json[65536];
    int length=snprintf(path,sizeof path,"%s/qwen36_meta.json",root);
    if (length<0 || (size_t)length>=sizeof path) return -1;
    FILE *f=lmb_plan_fopen(path); if (!f) return -1;
    size_t n=fread(json,1,sizeof json-1,f); int bad=ferror(f)||!feof(f);
    fclose(f);json[n]=0;
    const char *start=lmb_plan_space(json),*end;
    if (bad || !n || memchr(json,0,n) || *start!='{' ||
        !(end=lmb_plan_object_end(start)) || *lmb_plan_space(end)) return -1;
    memset(v,0,sizeof *v);v->m=m;
    char meta_type[64];
    if (lmb_json_member(json,"model_type")) {
        if (lmb_json_string(json,"model_type",meta_type,sizeof meta_type)) return -1;
        if (strcmp(meta_type,m->model_type) &&
            !( !strcmp(meta_type,"qwen3_5_moe") && !strcmp(m->model_type,"qwen3_5_moe_text")) &&
            !( !strcmp(meta_type,"qwen3_5") && !strcmp(m->model_type,"qwen3_5_text")) &&
            !( !strcmp(meta_type,"lfm3_vl") && !strcmp(m->model_type,"lfm3"))) return -1;
    }
    uint32_t hidden,layers,experts,topk,inter,shared,heads,kv,hd,qd,kd,vd,oi,gs;
    uint32_t vh,kh,kdim,vdim,convk,convdim,window,down_bits,down_gs;
#define Q36_NEED(key,var,lo,hi) if (lmb_plan_u32(json,key,lo,hi,&var)) return -1
    Q36_NEED("hidden",hidden,1,65536); Q36_NEED("n_layers",layers,1,LMB_PLAN_LAYER_MAX);
    Q36_NEED("num_experts",experts,0,1024); Q36_NEED("topk",topk,0,256);
    Q36_NEED("moe_inter",inter,0,65536); Q36_NEED("shared_inter",shared,0,1048576);
    Q36_NEED("q_heads",heads,1,65536); Q36_NEED("kv_heads",kv,1,65536);
    Q36_NEED("head_dim",hd,1,65536); Q36_NEED("q_head_dim",qd,1,131072);
    Q36_NEED("k_head_dim",kd,1,65536); Q36_NEED("v_head_dim",vd,1,65536);
    Q36_NEED("o_in",oi,1,1048576); Q36_NEED("expert_gs",gs,0,65536);
    Q36_NEED("dn_vheads",vh,0,65536); Q36_NEED("dn_kheads",kh,0,65536);
    Q36_NEED("dn_kdim",kdim,0,65536); Q36_NEED("dn_vdim",vdim,0,512);
    Q36_NEED("dn_convk",convk,0,65536); Q36_NEED("dn_conv_dim",convdim,0,1048576);
#undef Q36_NEED
    if (hidden!=m->hidden || layers!=m->layers || !m->vocab || experts!=m->experts ||
        topk!=m->experts_per_tok || heads%kv || kd!=vd || kd!=hd ||
        (qd!=hd && qd!=2*hd) || oi!=(uint64_t)heads*hd ||
        (experts ? (!topk || topk>experts || !inter) : (topk || inter || !shared))) return -1;
    if (lmb_q36_bool(json,"lfm3",0,&v->lfm3) || lmb_q36_bool(json,"attn_sinks",0,&v->sinks) ||
        lmb_q36_optional(json,"dense_inter",1048576,&v->dense_inter) ||
        lmb_q36_optional(json,"sliding_window",1048576,&window) ||
        lmb_q36_optional(json,"expert_down_bits",8,&down_bits) ||
        lmb_q36_optional(json,"expert_down_gs",65536,&down_gs)) return -1;
    /* Upstream's mixed container keeps packed int4 gate/up and int8 down,
     * with a separate grouping for the down scales; CPU unpacks into int8. */
    if (down_bits && down_bits!=8) return -1;
    v->mixed=down_bits!=0;
    unsigned lfm=!strcmp(m->model_type,"lfm3") || !strcmp(m->model_type,"lfm3_vl");
    if (v->lfm3!=lfm || (!strcmp(m->model_type,"flint") && !v->sinks)) return -1;
    if (lmb_q36_indices(json,"mlp_only_layers",layers,v->dense)) return -1;
    uint8_t no_rope[LMB_PLAN_LAYER_MAX]={0};
    if (lmb_q36_indices(json,"no_rope_layers",layers,no_rope)) return -1;
    v->shared_inter=shared;
    const char *p=lmb_json_member(json,"layer_types");
    if (!p || *p++!='[') return -1;
    unsigned linear=0;
    for (uint32_t i=0;i<layers;i++) {
        p=lmb_plan_space(p);
        if (!strncmp(p,"\"full_attention\"",16)) {v->full[i]=1;p+=16;}
        else if (!strncmp(p,"\"sliding_attention\"",19) && window) {v->full[i]=1;p+=19;}
        else if (!strncmp(p,"\"linear_attention\"",18)) {linear++;p+=18;}
        else if (v->lfm3 && !strncmp(p,"\"conv\"",6)) {linear++;p+=6;}
        else return -1;
        p=lmb_plan_space(p);
        if (*p++!=(i+1==layers ? ']' : ',')) return -1;
        if (v->dense[i] && !v->dense_inter) return -1;
    }
    uint64_t recurrent=0;
    if (linear) {
        if (convk<2) return -1;
        if (v->lfm3) {
            if (vh || convdim!=hidden) return -1;
            recurrent=1; /* upstream q36_rec_cells keeps a non-null placeholder */
        } else {
            if (!vh || !kh || vh%kh || !kdim || !vdim ||
                convdim!=2u*(uint64_t)kh*kdim+(uint64_t)vh*vdim) return -1;
            recurrent=(uint64_t)vh*kdim*vdim;
        }
        recurrent=lmb_size_add(recurrent,(uint64_t)convdim*(convk-1));
    }
    uint32_t dgs=down_bits ? down_gs : gs;
    v->scales=2u*(uint64_t)inter*(gs ? (hidden+gs-1)/gs : 1)+
        (uint64_t)hidden*(dgs ? (inter+dgs-1)/dgs : 1);
    v->expert_bytes=3u*(uint64_t)hidden*inter;
    v->slot=lmb_size_add(v->expert_bytes,v->scales*4);
    uint64_t H=hidden,DV=(uint64_t)vh*vdim;
    uint64_t counts[]={H,H,(uint64_t)experts*H,H*shared,H*shared,H*shared,H,hd,hd,experts,
        (uint64_t)heads*qd*H,(uint64_t)kv*kd*H,(uint64_t)kv*vd*H,H*oi,
        (uint64_t)convdim*H,DV*H,(uint64_t)vh*H,(uint64_t)vh*H,(uint64_t)convdim*convk,
        vh,vh,vdim,H*DV,0,0,0,heads,H,H,H*heads,3*H*H,H*convk,H*H};
    memcpy(v->counts,counts,sizeof counts);
    for (uint32_t i=0;i<layers;i++) {
        uint64_t req=(1ull<<Q36_IN)|(1ull<<Q36_POST);
        if (experts && !v->dense[i]) req|=1ull<<Q36_ROUTER;
        unsigned base=experts && !v->dense[i] ? Q36_SG : Q36_G;
        if (v->dense[i] || shared) req|=7ull<<base;
        req|=v->full[i] ? 15ull<<Q36_Q : v->lfm3 ? 7ull<<Q36_SC_IN : 511ull<<Q36_DQKV;
        if (v->sinks && v->full[i]) req|=1ull<<Q36_SINKS;
        if (v->lfm3) req|=(1ull<<Q36_POST_OP)|(1ull<<Q36_POST_FFN)|(v->full[i] ? 1ull<<Q36_ATTN_GATE : 0);
        v->expected[i]=req;
        m->memory[i].resident_bytes=experts && !v->dense[i] ? lmb_size_mul(v->slot,experts) : 0;
        m->memory[i].state_token_bytes=v->full[i] ? (uint64_t)kv*kd*8 : 0;
        m->memory[i].state_fixed_bytes=v->full[i] ? 0 : recurrent*4;
    }
    m->edge_resident_bytes=lmb_size_mul(lmb_size_add(lmb_size_mul((uint64_t)m->vocab*hidden,2),hidden),4);
    m->segment_fixed_bytes=lmb_size_mul(layers,4096u+(uint64_t)experts*256);
    uint64_t widest=hidden+(uint64_t)heads*qd+(uint64_t)kv*(kd+vd)+oi+convdim+
        (uint64_t)vh*(kdim+vdim)+inter+shared+v->dense_inter+experts;
    m->scratch_fixed_bytes=lmb_size_add(v->slot,lmb_size_add(lmb_size_mul(widest,128u*16u*4u),32u<<20));
    m->scratch_token_bytes=256u*4u;
    m->max_context=262144;
    m->heads=heads;m->kv_heads=kv;m->moe_intermediate=inter;
    m->memory_contract=1;
    return 0;
}
static int lmb_q36_variants_memory(const char *root,const char *cfg,LmbModelShape *m) {
    LmbQ36Variants *v=calloc(1,sizeof *v); if (!v) return -1;
    int rc=lmb_q36_variants_metadata(root,m,v);
    uint32_t declared_context;
    if (!rc && lmb_json_member(cfg,"max_position_embeddings")) {
        if (lmb_plan_u32(cfg,"max_position_embeddings",1,UINT32_MAX,&declared_context)) rc=-1;
        else if (declared_context<m->max_context) m->max_context=declared_context;
    }
    if (!rc) {
        v->experts=calloc(m->experts ? (size_t)m->layers*m->experts : 1,1);
        if (!v->experts) rc=-1;
    }
    if (!rc) rc=lmb_plan_tensors(root,lmb_q36_variant_tensor,v);
    if (v->edge!=7) rc=-1;
    for (unsigned i=0;i<m->layers && !rc;i++) {
        if ((v->seen[i]&v->expected[i])!=v->expected[i]) rc=-1;
        for (unsigned e=0;!v->dense[i] && e<m->experts;e++) if (v->experts[i*m->experts+e]!=3) rc=-1;
    }
    free(v->experts);free(v);return rc;
}
#endif
