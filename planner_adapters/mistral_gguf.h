#ifndef LMB_PLAN_MISTRAL_GGUF_H
#define LMB_PLAN_MISTRAL_GGUF_H
#include "olmoe_gguf.h"

typedef struct {
    uint32_t layers,hidden,vocab,heads,experts,topk,inter,dense,shared,first;
    uint32_t kv,rope,key,value,ih,idim,itop,context,original;
    uint8_t rotated[128],indexed[128];uint32_t source[128];
    double eps,ieps,theta,factor,fast,slow,scale,softcap;
    const char *rope_type;
} LmbMistralGguf;

static const LmbGgufValue *lmb_ml4_value(const LmbGguf *g,const char *suffix) {
    char name[128];int n=snprintf(name,sizeof name,"mistral-large4.%s",suffix);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_ml4_u32(const LmbGguf *g,const char *key,uint32_t fallback,uint32_t lo,uint32_t hi,uint32_t *out) {
    const LmbGgufValue *v=lmb_ml4_value(g,key);uint64_t n=fallback;
    if((v && lmb_gguf_integer(v,&n)) || n<lo || n>hi)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_ml4_array(const LmbGguf *g,const char *key,uint32_t n,uint32_t max,uint32_t *out) {
    const LmbGgufValue *v=lmb_ml4_value(g,key);
    if(!v || v->type!=9 || v->count!=n)return -1;
    unsigned width=lmb_gguf_scalar_bytes(v->element_type);
    if(!width || v->bytes!=(uint64_t)n*width)return -1;
    for(uint32_t i=0;i<n;i++) {
        LmbGgufValue one={.type=v->element_type,.bytes=width,.data=v->data+(size_t)i*width};uint64_t value;
        if(lmb_gguf_integer(&one,&value) || value>max)return -1;
        out[i]=(uint32_t)value;
    }
    return 0;
}
static int lmb_ml4_geometry(const LmbGguf *g,LmbMistralGguf *c) {
    memset(c,0,sizeof *c);
    const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"mistral-large4"))return -1;
#define ML(key,field,def,lo,hi) if(lmb_ml4_u32(g,key,def,lo,hi,&c->field))return -1
    ML("block_count",layers,0,1,128);ML("embedding_length",hidden,0,1,1048576);
    ML("attention.head_count",heads,0,1,1024);ML("expert_count",experts,0,1,4096);
    ML("expert_used_count",topk,0,1,64);ML("expert_feed_forward_length",inter,0,1,1048576);
    ML("feed_forward_length",dense,0,1,16777216);ML("expert_shared_count",shared,1,1,64);
    ML("leading_dense_block_count",first,0,0,c->layers);ML("attention.kv_lora_rank",kv,0,1,512);
    ML("rope.dimension_count",rope,0,2,256);ML("attention.key_length_mla",key,0,c->rope+1,65536);
    ML("attention.value_length_mla",value,0,1,65536);ML("attention.indexer.head_count",ih,0,1,1024);
    ML("attention.indexer.key_length",idim,0,c->rope,256);ML("attention.indexer.top_k",itop,0,1,1048576);
    ML("context_length",context,1048576,1,1048576);ML("rope.scaling.original_context_length",original,0,0,1048576);
#undef ML
    uint32_t dummy;
    if(c->topk>c->experts || c->rope%2 || lmb_ml4_u32(g,"expert_group_count",1,1,1,&dummy) ||
       lmb_ml4_u32(g,"expert_group_used_count",1,1,1,&dummy) || lmb_ml4_u32(g,"expert_gating_func",1,1,1,&dummy))return -1;
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || !tokens->count || tokens->count>1048576)return -1;
    c->vocab=(uint32_t)tokens->count;
    if(lmb_ml4_u32(g,"vocab_size",c->vocab,c->vocab,c->vocab,&dummy))return -1;
    uint32_t flags[128];
    if(lmb_ml4_array(g,"attention.rope_pattern",c->layers,1,flags))return -1;
    for(uint32_t i=0;i<c->layers;i++)c->rotated[i]=(uint8_t)flags[i];
    if(lmb_ml4_array(g,"attention.indexer.types",c->layers,1,flags))return -1;
    for(uint32_t i=0;i<c->layers;i++)c->indexed[i]=(uint8_t)flags[i];
    if(!c->indexed[0] || lmb_ml4_array(g,"attention.kv_source_map",c->layers,c->layers-1,c->source))return -1;
    for(uint32_t i=0;i<c->layers;i++)if(c->source[i]>i || c->rotated[i]!=c->rotated[c->source[i]])return -1;
#define MF(key,field,def) if(lmb_gguf_number(lmb_ml4_value(g,key),def,&c->field) || c->field>3.402823466e38)return -1
    MF("attention.layer_norm_rms_epsilon",eps,1e-5);MF("attention.layer_norm_epsilon",ieps,1e-6);
    MF("rope.freq_base",theta,10000);MF("rope.scaling.factor",factor,1);
    MF("rope.scaling.yarn_beta_fast",fast,32);MF("rope.scaling.yarn_beta_slow",slow,1);
    MF("expert_weights_scale",scale,1);MF("final_logit_softcapping",softcap,0);
#undef MF
    if(c->eps<=0 || c->ieps<=0 || c->theta<=1 || c->factor<=0 || c->fast<=0 || c->slow<=0 || c->scale<=0 || c->softcap<0)return -1;
    const LmbGgufValue *rt=lmb_ml4_value(g,"rope.scaling.type");
    c->rope_type=rt?lmb_gguf_text(rt):"default";
    if(!c->rope_type || (strcmp(c->rope_type,"default") && strcmp(c->rope_type,"yarn")))return -1;
    const LmbGgufValue *norm=lmb_ml4_value(g,"expert_weights_norm");uint64_t n;
    if(norm && (lmb_gguf_integer(norm,&n) || n!=1))return -1;
    return 0;
}

typedef struct {const LmbGguf *g;uint8_t *seen;uint64_t largest;} LmbMistralInventory;
static int lmb_ml4_item(const LmbGgufValue *v,uint32_t i,uint64_t *out) {
    if(!v || v->type!=9 || i>=v->count)return -1;
    unsigned width=lmb_gguf_scalar_bytes(v->element_type);
    if(!width || (uint64_t)(i+1)*width>v->bytes)return -1;
    LmbGgufValue item={.type=v->element_type,.bytes=width,.data=v->data+(size_t)i*width};
    return lmb_gguf_integer(&item,out);
}
static int lmb_ml4_config_matches(const char *cfg,const LmbMistralGguf *c) {
    const char *keys[]={"num_hidden_layers","hidden_size","vocab_size","num_attention_heads","n_routed_experts",
        "num_experts_per_tok","moe_intermediate_size","intermediate_size","n_shared_experts","first_k_dense_replace",
        "q_lora_rank","kv_lora_rank","qk_rope_head_dim","qk_nope_head_dim","v_head_dim","index_n_heads",
        "index_head_dim","index_topk","n_group","topk_group","max_position_embeddings"};
    uint32_t values[]={c->layers,c->hidden,c->vocab,c->heads,c->experts,c->topk,c->inter,c->dense,c->shared,c->first,
        0,c->kv,c->rope,c->key-c->rope,c->value,c->ih,c->idim,c->itop,1,1,c->context};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++) {
        uint32_t value;if(lmb_plan_u32(cfg,keys[i],values[i],values[i],&value))return -1;
    }
    const char *floats[]={"rms_norm_eps","indexer_norm_eps","routed_scaling_factor","final_logit_softcapping"};
    double real[]={c->eps,c->ieps,c->scale,c->softcap};
    for(unsigned i=0;i<4;i++) {double value;if(lmb_plan_number(cfg,floats[i],-1,&value) || (float)value!=(float)real[i])return -1;}
    char text[64];
    if(lmb_json_string(cfg,"model_type",text,sizeof text) || strcmp(text,"mistral_large4") ||
       lmb_json_string(cfg,"scoring_func",text,sizeof text) || strcmp(text,"softmax"))return -1;
    const char *norm=lmb_json_member(cfg,"norm_topk_prob");
    if(!norm || strncmp(norm,"true",4) || (*lmb_plan_space(norm+4)!=',' && *lmb_plan_space(norm+4)!='}'))return -1;
    const char *rope=lmb_json_member(cfg,"rope_parameters");
    if(!rope || *rope!='{' || lmb_json_string(rope,"rope_type",text,sizeof text) || strcmp(text,c->rope_type))return -1;
    const char *rk[]={"rope_theta","factor","beta_fast","beta_slow"};double rv[]={c->theta,c->factor,c->fast,c->slow};
    for(unsigned i=0;i<4;i++){double value;if(lmb_plan_number(rope,rk[i],-1,&value) || (float)value!=(float)rv[i])return -1;}
    uint32_t original;if(lmb_plan_u32(rope,"original_max_position_embeddings",c->original,c->original,&original))return -1;
    uint64_t list[128];unsigned n;
    if(lmb_plan_array(lmb_json_member(cfg,"no_rope_layers"),list,128,&n) || n!=c->layers)return -1;
    for(unsigned i=0;i<n;i++)if(list[i]!=c->rotated[i])return -1;
    if(lmb_plan_array(lmb_json_member(cfg,"kv_source_layers"),list,128,&n) || n!=c->layers)return -1;
    for(unsigned i=0;i<n;i++)if(list[i]!=c->source[i])return -1;
    uint8_t indexed[128];
    if(lmb_plan_kinds(cfg,"indexer_types","shared","full",indexed,c->layers) || memcmp(indexed,c->indexed,c->layers))return -1;
    return 0;
}
static int lmb_ml4_weight(LmbMistralInventory *v,const char *name,uint32_t rank,
                          uint64_t x,uint64_t y,uint64_t z,int matrix,uint64_t *cost) {
    const LmbGgufTensor *t=lmb_gguf_tensor(v->g,name);
    if(!t || t->rank!=rank || t->shape[0]!=x || (rank>1 && t->shape[1]!=y) || (rank>2 && t->shape[2]!=z) ||
       !LMB_GGUF_TYPES[t->type].decodable || v->seen[t-v->g->tensors])return -1;
    v->seen[t-v->g->tensors]=1;
    uint64_t elements=lmb_size_mul(x,lmb_size_mul(y,z));
    uint64_t bytes=lmb_size_mul(elements,4);
    if(matrix) {
        LmbGgufTensor flat=*t;flat.rank=2;flat.shape[1]=lmb_size_mul(y,z);
        bytes=lmb_gguf_held_bytes(&flat,1,1);
        if(t->type==0 || t->type==1 || t->type==30) {
            /* Also cover float-source quantization choices and row scales. */
            uint64_t floating=lmb_size_mul(elements,5);if(floating>bytes)bytes=floating;
        }
    }
    if(bytes==UINT64_MAX)return -1;
    if(bytes>v->largest)v->largest=bytes;
    *cost=lmb_size_add(*cost,bytes);return *cost==UINT64_MAX?-1:0;
}
static int lmb_mistral_gguf_memory(const LmbGguf *g,const char *cfg,LmbModelShape *m) {
    LmbMistralGguf c;if(lmb_ml4_geometry(g,&c))return -1;
    if(lmb_ml4_config_matches(cfg,&c))return -1;
    if(m->layers!=c.layers || m->hidden!=c.hidden || m->vocab!=c.vocab || m->experts!=c.experts || m->experts_per_tok!=c.topk)return -1;
    const char *layout=lmb_gguf_text(lmb_gguf_get(g,"colibri.layout"));
    int packed=layout && !strcmp(layout,"expert-packed-v1");
    if(layout && !packed)return -1;
    const LmbGgufValue *pt=lmb_gguf_get(g,"colibri.expert_types"),*pb=lmb_gguf_get(g,"colibri.expert_bytes"),
        *ps=lmb_gguf_get(g,"colibri.expert_stride"),*pd=lmb_gguf_get(g,"colibri.expert_dims");
    if(packed && (!pt || !pb || !ps || !pd || pt->type!=9 || pt->element_type!=4 || pt->count!=3u*c.layers ||
        pb->type!=9 || pb->element_type!=10 || pb->count!=3u*c.layers ||
        ps->type!=9 || ps->element_type!=10 || ps->count!=c.layers ||
        pd->type!=9 || pd->element_type!=10 || pd->count!=6u*c.layers))return -1;
    LmbMistralInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    int rc=-1;uint64_t H=c.hidden;char name[128];
#define MW(n,r,x,y,z,mat,budget) if(lmb_ml4_weight(&v,n,r,x,y,z,mat,budget))goto done
    MW("token_embd.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    MW("output.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    MW("output_norm.weight",1,H,1,1,0,&m->edge_resident_bytes);
    uint64_t rope_bytes=0;
    if(lmb_gguf_tensor(g,"rope_freqs.weight"))MW("rope_freqs.weight",1,c.rope/2,1,1,0,&rope_bytes);
    m->segment_fixed_bytes=rope_bytes;
    uint64_t maxexpert=0;
    for(uint32_t l=0;l<c.layers;l++) {
        uint64_t *cost=&m->memory[l].resident_bytes;
#define LW(s,r,x,y,z,mat) do {snprintf(name,sizeof name,"blk.%u.%s",l,s);MW(name,r,x,y,z,mat,cost);}while(0)
        LW("attn_norm.weight",1,H,1,1,0);LW("ffn_norm.weight",1,H,1,1,0);
        LW("attn_q.weight",2,H,(uint64_t)c.heads*c.key,1,1);
        uint32_t R=c.rotated[l]?c.rope:0;
        if(c.source[l]==l) {
            LW("attn_kv_a_mqa.weight",2,H,c.kv+R,1,1);LW("attn_kv_a_norm.weight",1,c.kv,1,1,0);
        }
        LW("attn_k_b.weight",3,c.key-R,c.kv,c.heads,1);
        LW("attn_v_b.weight",3,c.kv,c.value,c.heads,1);
        LW("attn_output.weight",2,(uint64_t)c.heads*c.value,H,1,1);
        LW("attn_ssmax.weight",1,c.heads,1,1,0);LW("attn_ssmax.bias",1,c.heads,1,1,0);
        if(c.indexed[l]) {
            LW("indexer.attn_q_b.weight",2,H,(uint64_t)c.ih*c.idim,1,1);
            LW("indexer.attn_k.weight",2,H,c.idim,1,1);LW("indexer.proj.weight",2,H,c.ih,1,1);
            LW("indexer.q_norm.weight",1,H,1,1,0);LW("indexer.q_norm.bias",1,H,1,1,0);
            LW("indexer.k_norm.weight",1,c.idim,1,1,0);LW("indexer.k_norm.bias",1,c.idim,1,1,0);
            LW("indexer.ssmax.weight",1,c.ih,1,1,0);LW("indexer.ssmax.bias",1,c.ih,1,1,0);
        }
        uint32_t I=l<c.first?c.dense:c.inter*c.shared;
        LW(l<c.first?"ffn_gate.weight":"ffn_gate_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_up.weight":"ffn_up_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_down.weight":"ffn_down_shexp.weight",2,I,H,1,1);
        if(l>=c.first) {
            LW("ffn_gate_inp.weight",2,H,c.experts,1,0);
            uint64_t before=*cost;
            if(packed) {
                snprintf(name,sizeof name,"blk.%u.ffn_exps_colibri",l);
                const LmbGgufTensor *t=lmb_gguf_tensor(g,name);uint64_t stride,total=0;
                if(!t || t->rank!=2 || t->type!=24 || lmb_ml4_item(ps,l,&stride) || !stride ||
                    t->shape[0]!=stride || t->shape[1]!=c.experts)goto done;
                v.seen[t-g->tensors]=1;
                for(unsigned k=0;k<3;k++) {
                    uint64_t type,bytes;
                    if(lmb_ml4_item(pt,3*l+k,&type) || lmb_ml4_item(pb,3*l+k,&bytes) ||
                       type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES || !LMB_GGUF_TYPES[type].decodable)goto done;
                    const LmbGgufType *q=&LMB_GGUF_TYPES[type];
                    uint64_t cols=k==2?c.inter:H,rows=k==2?H:c.inter,x,y;
                    if(lmb_ml4_item(pd,6*l+2*k,&x) || lmb_ml4_item(pd,6*l+2*k+1,&y) || x!=cols || y!=rows ||
                        cols%q->block || bytes!=lmb_size_mul(cols/q->block,q->bytes*rows))goto done;
                    LmbGgufTensor weight={.type=(uint32_t)type,.rank=2,.shape={cols,rows}};
                    uint64_t one=lmb_gguf_held_bytes(&weight,1,1);
                    if(type==0 || type==1 || type==30)one=lmb_size_mul(lmb_size_mul(cols,rows),5);
                    *cost=lmb_size_add(*cost,lmb_size_mul(one,c.experts));
                    total=lmb_size_add(total,bytes);
                    if(one>v.largest)v.largest=one;
                }
                if(total>stride || *cost==UINT64_MAX)goto done;
            } else {
                LW("ffn_gate_exps.weight",3,H,c.inter,c.experts,1);
                LW("ffn_up_exps.weight",3,H,c.inter,c.experts,1);
                LW("ffn_down_exps.weight",3,c.inter,H,c.experts,1);
            }
            uint64_t one=(*cost-before)/c.experts;if(one>maxexpert)maxexpert=one;
        }
        m->memory[l].state_token_bytes=((uint64_t)c.kv+c.rope+(c.indexed[l]?c.idim:0))*4u;
#undef LW
    }
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i])goto done;
    uint64_t headers=lmb_size_add(lmb_size_mul(g->header_bytes,6),lmb_size_mul(g->ntensors,1024));
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,headers);
    m->edge_scratch_fixed_bytes=lmb_size_add(v.largest,((uint64_t)c.vocab+H)*4);
    m->segment_fixed_bytes=lmb_size_add(m->segment_fixed_bytes,lmb_size_add(headers,(uint64_t)c.layers*(8192u+(uint64_t)c.experts*2048u)));
    uint64_t wide=H+(uint64_t)c.heads*(c.key+c.value)+c.kv+c.dense+(uint64_t)c.shared*c.inter+
        (uint64_t)c.topk*(H+c.inter)+c.experts+(uint64_t)c.ih*c.idim;
    m->scratch_fixed_bytes=lmb_size_add(wide*512u*16u*4u,lmb_size_add(v.largest,lmb_size_mul(maxexpert,c.experts<256?c.experts:256)));
    m->scratch_fixed_bytes=lmb_size_add(m->scratch_fixed_bytes,lmb_size_mul(maxexpert,64));
    m->scratch_token_bytes=256u*32u;m->heads=c.heads;m->kv_heads=c.heads;m->moe_intermediate=c.inter;
    m->max_context=c.context;m->memory_contract=1;rc=0;
done:free(v.seen);return rc;
#undef MW
}
#endif
