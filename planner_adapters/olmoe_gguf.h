#ifndef LMB_PLAN_OLMOE_GGUF_H
#define LMB_PLAN_OLMOE_GGUF_H
#include "gguf_model.h"

/* Maximum loaded representation over supported CPU builds. Dense floating
 * sources expand to f32; block matrices retain blocks, with room for the
 * rows16 packing when the backend implements it. Experts keep native blocks
 * (including floating blocks), padded by the runtime's per-range slot sizes. */
static uint64_t lmb_gguf_held_bytes(const LmbGgufTensor *t,int dense,int rows16) {
    const LmbGgufType *q=&LMB_GGUF_TYPES[t->type];
    if(!q->decodable) return UINT64_MAX;
    uint64_t rows=t->rank>1?t->shape[1]:1, columns=t->shape[0];
    if(dense && (t->type==0 || t->type==1 || t->type==30))
        return lmb_size_mul(lmb_size_mul(rows,columns),4);
    uint64_t cost=lmb_size_mul(columns/q->block,lmb_size_mul(q->bytes,rows));
    if(rows16 && q->rows16_bytes && !(rows%16)) {
        uint64_t packed=lmb_size_mul(columns/q->block,lmb_size_mul(q->rows16_bytes,rows/16));
        if(packed>cost)cost=packed;
    }
    return cost;
}

static int lmb_olmoe_gguf_memory(const LmbGguf *g,LmbModelShape *m) {
    const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"olmoe"))return -1;
    const char *keys[]={"olmoe.embedding_length","olmoe.block_count","olmoe.expert_count",
        "olmoe.expert_used_count","olmoe.feed_forward_length","olmoe.attention.head_count",
        "olmoe.attention.head_count_kv"};
    uint64_t expected[]={m->hidden,m->layers,m->experts,m->experts_per_tok,m->moe_intermediate,m->heads,m->kv_heads};
    for(unsigned i=0;i<sizeof keys/sizeof *keys;i++) {
        uint64_t v;const LmbGgufValue *value=lmb_gguf_get(g,keys[i]);
        if(i==6 && !value){if(m->kv_heads!=m->heads)return -1;continue;}
        if(i==4 && lmb_gguf_get(g,"olmoe.expert_feed_forward_length"))value=lmb_gguf_get(g,"olmoe.expert_feed_forward_length");
        if(lmb_gguf_integer(value,&v) || v!=expected[i])return -1;
    }
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || tokens->count!=m->vocab)return -1;
    uint16_t seen[LMB_PLAN_LAYER_MAX]={0};unsigned edge=0;
    uint64_t largest=0;
    static const char *const names[]={"attn_norm.weight","ffn_norm.weight","attn_q.weight",
        "attn_k.weight","attn_v.weight","attn_output.weight","attn_q_norm.weight",
        "attn_k_norm.weight","ffn_gate_inp.weight","ffn_gate_exps.weight",
        "ffn_up_exps.weight","ffn_down_exps.weight"};
    for(uint32_t i=0;i<g->ntensors;i++) {
        const LmbGgufTensor *t=&g->tensors[i];
        int e=!strcmp(t->name,"token_embd.weight")?0:!strcmp(t->name,"output.weight")?1:
            !strcmp(t->name,"output_norm.weight")?2:-1;
        int norm=0,expert=-1;uint64_t rows,columns=m->hidden,layer=0;unsigned field=0;
        if(e>=0) {
            if(edge&(1u<<e))return -1;
            edge|=1u<<e;norm=e==2;rows=norm?1:m->vocab;
        } else {
            if(strncmp(t->name,"blk.",4))return -1;
            const char *name=lmb_plan_uint(t->name+4,&layer);
            if(!name || *name++!='.' || layer>=m->layers)return -1;
            for(;field<12 && strcmp(name,names[field]);field++);
            if(field==12 || (seen[layer]&(1u<<field)))return -1;
            seen[layer]|=1u<<field;
            norm=field==0 || field==1 || field==6 || field==7;
            rows=norm?1:field==8?m->experts:m->hidden;
            if(field>=9) {
                expert=(int)field-9;rows=expert==2?m->hidden:m->moe_intermediate;
                columns=expert==2?m->moe_intermediate:m->hidden;
            }
        }
        if(t->rank!=(norm?1u:expert>=0?3u:2u) || t->shape[0]!=columns ||
           (!norm && t->shape[1]!=rows) || (expert>=0 && t->shape[2]!=m->experts))return -1;
        if(norm && t->type!=0 && t->type!=1 && t->type!=30)return -1;
        uint64_t cost=lmb_gguf_held_bytes(t,expert<0,e!=0);
        if(cost==UINT64_MAX)return -1;
        if(e>=0) {
            m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,cost);
            if(cost>m->edge_scratch_fixed_bytes)m->edge_scratch_fixed_bytes=cost;
        } else {
            if(expert>=0)m->memory[layer].expert_slot_bytes[expert]=cost;
            else m->memory[layer].resident_bytes=lmb_size_add(m->memory[layer].resident_bytes,cost);
            if(cost>largest)largest=cost;
        }
    }
    if(edge!=7)return -1;
    for(uint32_t l=0;l<m->layers;l++) {
        if(seen[l]!=4095)return -1;
        m->memory[l].state_token_bytes=(uint64_t)m->hidden*8;
    }
    /* The complete name index remains on each process, including references
     * to layers it does not own. Only assigned tensor payloads become RAM. */
    uint64_t index=lmb_size_mul((uint64_t)m->layers*(9u+3u*m->experts)+3u,512u);
    m->segment_fixed_bytes=lmb_size_add(index,(uint64_t)m->layers*(8192u+(uint64_t)m->experts*512u));
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,index);
    uint64_t metadata=lmb_size_mul(g->header_bytes,6u);
    m->scratch_fixed_bytes=lmb_size_add(largest,
        lmb_size_mul((uint64_t)m->hidden*8+m->moe_intermediate*3u+m->experts,128u*4u));
    if(metadata>m->scratch_fixed_bytes)m->scratch_fixed_bytes=metadata;
    if(metadata>m->edge_scratch_fixed_bytes)m->edge_scratch_fixed_bytes=metadata;
    m->scratch_token_bytes=256u*4u;
    uint64_t context=4096;const LmbGgufValue *limit=lmb_gguf_get(g,"olmoe.context_length");
    if(limit && (lmb_gguf_integer(limit,&context) || !context))return -1;
    m->max_context=context<4096?(uint32_t)context:4096;m->memory_contract=1;
    return m->segment_fixed_bytes==UINT64_MAX || m->edge_resident_bytes==UINT64_MAX ||
        m->scratch_fixed_bytes==UINT64_MAX ? -1:0;
}
#endif
