#ifndef LUMABRI_PLAN_OLMOE_H
#define LUMABRI_PLAN_OLMOE_H
/* The pinned Segment runtime consumes merged int8 experts, not arbitrary HF
 * expert matrices. Dense tensors are expanded to f32 even from BF16/F16. */
typedef struct {
    LmbModelShape *m;
    uint16_t seen[LMB_PLAN_LAYER_MAX];
    uint8_t edge_seen, *experts;
    uint32_t inter;
    uint64_t largest;
} LmbOlmoeInventory;

static int lmb_olmoe_tensor(const LmbPlanTensor *t, void *opaque) {
    LmbOlmoeInventory *v=opaque;
    LmbModelShape *m=v->m;
    int floating=!strcmp(t->dtype,"F32") || !strcmp(t->dtype,"BF16") || !strcmp(t->dtype,"F16");
    int edge=!strcmp(t->name,"model.embed_tokens.weight") ? 0 :
        !strcmp(t->name,"lm_head.weight") ? 1 : !strcmp(t->name,"model.norm.weight") ? 2 : -1;
    uint64_t expected, cost;
    if(edge>=0) {
        expected=edge<2 ? (uint64_t)m->vocab*m->hidden : m->hidden;
        if(!floating || t->elements!=expected || (v->edge_seen&(1u<<edge)) ||
           t->rank!=(edge<2 ? 2u : 1u) || t->shape[0]!=(edge<2 ? m->vocab : m->hidden) ||
           (edge<2 && t->shape[1]!=m->hidden)) return -1;
        v->edge_seen|=1u<<edge;
        cost=lmb_size_mul(expected,4);
        m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,cost);
        if(cost>m->edge_scratch_fixed_bytes) m->edge_scratch_fixed_bytes=cost;
        return m->edge_resident_bytes==UINT64_MAX ? -1 : 0;
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
            expected=lmb_size_mul((uint64_t)v->inter*m->hidden,3);
            if((strcmp(t->dtype,"I8") && strcmp(t->dtype,"U8")) || t->bytes!=expected) return -1;
            cost=expected; bit=1;
        } else if(!strcmp(kind,"qs")) {
            expected=(uint64_t)v->inter*2+m->hidden;
            if(!floating || t->elements!=expected) return -1;
            cost=lmb_size_mul(expected,4); bit=2;
        } else return -1;
        uint8_t *seen=&v->experts[layer*m->experts+expert];
        if(*seen&bit) return -1;
        *seen|=bit;
    } else {
        static const char *const names[]={"input_layernorm.weight","post_attention_layernorm.weight",
            "self_attn.q_proj.weight","self_attn.k_proj.weight","self_attn.v_proj.weight",
            "self_attn.o_proj.weight","self_attn.q_norm.weight","self_attn.k_norm.weight","mlp.gate.weight"};
        unsigned i;
        for(i=0;i<sizeof names/sizeof *names;i++) if(!strcmp(field,names[i])) break;
        if(i==sizeof names/sizeof *names) return -1;
        int matrix=(i>=2 && i<=5) || i==8;
        uint64_t rows=i==8 ? m->experts : m->hidden;
        expected=matrix ? rows*m->hidden : m->hidden;
        if(!floating || t->elements!=expected || (v->seen[layer]&(1u<<i)) ||
           t->rank!=(matrix ? 2u : 1u) || t->shape[0]!=rows || (matrix && t->shape[1]!=m->hidden)) return -1;
        v->seen[layer]|=1u<<i;
        cost=lmb_size_mul(expected,4);
    }
    if(cost>v->largest) v->largest=cost;
    m->memory[layer].resident_bytes=lmb_size_add(m->memory[layer].resident_bytes,cost);
    return m->memory[layer].resident_bytes==UINT64_MAX ? -1 : 0;
}

static int lmb_olmoe_memory(const char *root,const char *cfg,LmbModelShape *m) {
    uint32_t h,layers,heads,kv,inter,experts,k,vocab;
#define OLM_NEED(key,dst,lo,hi) if(lmb_plan_u32(cfg,key,lo,hi,&dst)) return -1
    OLM_NEED("hidden_size",h,2,1048576); OLM_NEED("num_hidden_layers",layers,1,LMB_PLAN_LAYER_MAX);
    OLM_NEED("num_attention_heads",heads,1,65536); OLM_NEED("num_key_value_heads",kv,1,65536);
    OLM_NEED("intermediate_size",inter,1,16777216); OLM_NEED("num_experts",experts,1,4096);
    OLM_NEED("num_experts_per_tok",k,1,64); OLM_NEED("vocab_size",vocab,1,16777216);
#undef OLM_NEED
    /* Attention allocates full MHA; GQA-shaped checkpoints are not compatible
     * with this runtime just because config contains num_key_value_heads. */
    if(h!=m->hidden || layers!=m->layers || vocab!=m->vocab || h%heads || (h/heads)%2 ||
       kv!=heads || k>experts) return -1;
    m->experts=experts; m->experts_per_tok=k; m->moe_intermediate=inter;
    LmbOlmoeInventory v={.m=m,.inter=inter};
    v.experts=calloc((size_t)layers*experts,1);
    if(!v.experts) return -1;
    int rc=lmb_plan_tensors(root,lmb_olmoe_tensor,&v);
    if(v.edge_seen!=7) rc=-1;
    for(uint32_t i=0;i<layers && !rc;i++) {
        if(v.seen[i]!=511) rc=-1;
        for(uint32_t e=0;e<experts;e++) if(v.experts[i*experts+e]!=3) rc=-1;
        m->memory[i].state_token_bytes=(uint64_t)h*8;
    }
    free(v.experts);
    if(rc) return -1;
    /* Full resident expert bank plus metadata. No disk claim follows merely
     * from the runtime having an LRU. Prefill supports at most 128 rows. */
    m->segment_fixed_bytes=(uint64_t)layers*(8192u+(uint64_t)experts*512u);
    m->scratch_fixed_bytes=lmb_size_add(v.largest,
        lmb_size_mul((uint64_t)h*8+inter*3u+experts,128u*4u));
    m->scratch_token_bytes=256u*4u; /* bounded CPU attention worker team */
    m->max_context=4096; m->memory_contract=1;
    return m->scratch_fixed_bytes==UINT64_MAX ? -1 : 0;
}
#endif
