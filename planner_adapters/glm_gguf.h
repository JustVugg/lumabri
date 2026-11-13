#ifndef LMB_PLAN_GLM_GGUF_H
#define LMB_PLAN_GLM_GGUF_H
#include "gguf_inventory.h"

typedef struct {
    uint32_t layers,blocks,nextn,hidden,vocab,heads,experts,topk,inter,dense,shared,first;
    uint32_t query,kv,rope,key,value,ih,idim,itop,context,normalized;
    uint8_t indexed[128];
    double eps,theta,scale;
} LmbGlmGguf;
static const LmbGgufValue *lmb_glmd_value(const LmbGguf *g,const char *key) {
    char name[128];int n=snprintf(name,sizeof name,"glm-dsa.%s",key);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_glmd_uint(const LmbGguf *g,const char *key,uint32_t def,uint32_t lo,uint32_t hi,uint32_t *out) {
    const LmbGgufValue *v=lmb_glmd_value(g,key);uint64_t n=def;
    if((v && lmb_gguf_integer(v,&n)) || n<lo || n>hi)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_glmd_geometry(const LmbGguf *g,LmbGlmGguf *c) {
    memset(c,0,sizeof *c);const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"glm-dsa"))return -1;
#define GU(key,field,def,lo,hi) if(lmb_glmd_uint(g,key,def,lo,hi,&c->field))return -1
    GU("block_count",blocks,0,1,128);GU("nextn_predict_layers",nextn,0,0,1);
    char name[128];snprintf(name,sizeof name,"blk.%u.nextn.eh_proj.weight",c->blocks-1);
    if(!lmb_gguf_tensor(g,name))c->nextn=0;
    c->layers=c->blocks-c->nextn;if(!c->layers)return -1;
    GU("embedding_length",hidden,0,32,1048576);GU("attention.head_count",heads,0,1,1024);
    GU("expert_count",experts,0,1,4096);GU("expert_used_count",topk,0,1,64);
    GU("expert_feed_forward_length",inter,0,32,1048576);GU("feed_forward_length",dense,0,32,16777216);
    GU("expert_shared_count",shared,1,1,64);GU("leading_dense_block_count",first,0,0,c->layers);
    GU("attention.q_lora_rank",query,0,32,1048576);GU("attention.kv_lora_rank",kv,0,32,1048576);
    GU("rope.dimension_count",rope,0,2,256);GU("attention.key_length_mla",key,0,c->rope+1,65536);
    GU("attention.value_length_mla",value,0,1,65536);GU("attention.indexer.head_count",ih,0,1,1024);
    GU("attention.indexer.key_length",idim,0,c->rope,256);GU("attention.indexer.top_k",itop,0,1,1048576);
    GU("context_length",context,202752,1,1048576);GU("expert_weights_norm",normalized,1,0,1);
#undef GU
    uint32_t dummy;
    if(c->rope%2 || c->topk>c->experts || lmb_glmd_uint(g,"expert_group_count",1,1,1,&dummy) ||
       lmb_glmd_uint(g,"expert_group_used_count",1,1,1,&dummy) || lmb_glmd_uint(g,"expert_gating_func",2,2,2,&dummy))return -1;
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || !tokens->count || tokens->count>1048576)return -1;
    c->vocab=(uint32_t)tokens->count;
    if(lmb_glmd_uint(g,"vocab_size",c->vocab,c->vocab,c->vocab,&dummy))return -1;
    for(uint32_t l=0;l<c->layers;l++) {
        snprintf(name,sizeof name,"blk.%u.indexer.attn_q_b.weight",l);
        c->indexed[l]=lmb_gguf_tensor(g,name)!=NULL;
    }
    if(!c->indexed[0])return -1;
    if(lmb_gguf_number(lmb_glmd_value(g,"attention.layer_norm_rms_epsilon"),1e-5,&c->eps) ||
       lmb_gguf_number(lmb_glmd_value(g,"rope.freq_base"),10000,&c->theta) ||
       lmb_gguf_number(lmb_glmd_value(g,"expert_weights_scale"),1,&c->scale) ||
       c->eps<=0 || c->eps>3.402823466e38 || c->theta<=1 || c->theta>3.402823466e38 ||
       c->scale<=0 || c->scale>3.402823466e38)return -1;
    return 0;
}
static int lmb_glmd_config_matches(const char *cfg,const LmbGlmGguf *c) {
    const char *keys[]={"num_hidden_layers","hidden_size","vocab_size","num_attention_heads","n_routed_experts",
        "num_experts_per_tok","moe_intermediate_size","intermediate_size","n_shared_experts","first_k_dense_replace",
        "q_lora_rank","kv_lora_rank","qk_rope_head_dim","qk_nope_head_dim","v_head_dim","index_n_heads",
        "index_head_dim","index_topk","n_group","topk_group","max_position_embeddings"};
    uint32_t values[]={c->layers,c->hidden,c->vocab,c->heads,c->experts,c->topk,c->inter,c->dense,c->shared,c->first,
        c->query,c->kv,c->rope,c->key-c->rope,c->value,c->ih,c->idim,c->itop,1,1,c->context};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++) {
        uint32_t value;if(lmb_plan_u32(cfg,keys[i],values[i],values[i],&value))return -1;
    }
    char text[64];const char *rope=lmb_json_member(cfg,"rope_parameters");
    if(lmb_json_string(cfg,"model_type",text,sizeof text) || strcmp(text,"glm_moe_dsa") ||
       lmb_json_string(cfg,"scoring_func",text,sizeof text) || strcmp(text,"sigmoid") ||
       !rope || *rope!='{' || lmb_json_string(rope,"rope_type",text,sizeof text) || strcmp(text,"default"))return -1;
    const char *keysf[]={"rms_norm_eps","routed_scaling_factor","rope_theta"};double valuesf[]={c->eps,c->scale,c->theta};
    for(unsigned i=0;i<3;i++) {double value;if(lmb_plan_number(i==2?rope:cfg,keysf[i],-1,&value) || (float)value!=(float)valuesf[i])return -1;}
    const char *norm=lmb_json_member(cfg,"norm_topk_prob"),*expect=c->normalized?"true":"false";
    if(!norm || strncmp(norm,expect,strlen(expect)))return -1;
    norm=lmb_plan_space(norm+strlen(expect));if(*norm!=',' && *norm!='}')return -1;
    uint8_t indexed[128];
    if(lmb_plan_kinds(cfg,"indexer_types","shared","full",indexed,c->layers) || memcmp(indexed,c->indexed,c->layers))return -1;
    return 0;
}
static int lmb_glmd_matrix(LmbGgufInventory *v,const char *name,uint64_t cols,uint64_t rows,uint64_t heads,uint64_t *cost) {
    const LmbGgufTensor *t=lmb_gguf_tensor(v->g,name);
    if(!t)return -1;
    if(t->rank==3)return lmb_gguf_inventory_weight(v,name,3,cols,rows,heads,1,cost);
    return lmb_gguf_inventory_weight(v,name,2,cols,rows*heads,1,1,cost);
}
static int lmb_glmd_gguf_memory(const LmbGguf *g,const char *cfg,LmbModelShape *m) {
    LmbGlmGguf c;if(lmb_glmd_geometry(g,&c) || lmb_glmd_config_matches(cfg,&c))return -1;
    if(m->layers!=c.layers || m->hidden!=c.hidden || m->vocab!=c.vocab || m->experts!=c.experts || m->experts_per_tok!=c.topk)return -1;
    if(!lmb_gguf_tensor(g,"output.weight")) {
        const char *tie=lmb_json_member(cfg,"tie_word_embeddings");
        if(!tie || strncmp(tie,"true",4))return -1;
        tie=lmb_plan_space(tie+4);if(*tie!=',' && *tie!='}')return -1;
    }
    uint64_t width=(uint64_t)c.hidden+2+c.itop;
    if(width>1048576)return -1;
    m->boundary_width=(uint32_t)width;
    const char *layout=lmb_gguf_text(lmb_gguf_get(g,"colibri.layout"));
    int packed=layout && !strcmp(layout,"expert-packed-v1");if(layout && !packed)return -1;
    const LmbGgufValue *pt=lmb_gguf_get(g,"colibri.expert_types"),*pb=lmb_gguf_get(g,"colibri.expert_bytes"),
        *ps=lmb_gguf_get(g,"colibri.expert_stride"),*pd=lmb_gguf_get(g,"colibri.expert_dims");
    if(packed && (!pt || !pb || !ps || !pd || pt->type!=9 || pt->element_type!=4 || pt->count!=3u*c.layers ||
        pb->type!=9 || pb->element_type!=10 || pb->count!=3u*c.layers ||
        ps->type!=9 || ps->element_type!=10 || ps->count!=c.layers ||
        pd->type!=9 || pd->element_type!=10 || pd->count!=6u*c.layers))return -1;
    LmbGgufInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    int rc=-1;uint64_t H=c.hidden;char name[128];
#define GW(n,r,x,y,z,mat,budget) if(lmb_gguf_inventory_weight(&v,n,r,x,y,z,mat,budget))goto done
    GW("token_embd.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    if(lmb_gguf_tensor(g,"output.weight")) {
        GW("output.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    } else m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,(uint64_t)H*c.vocab*5);
    GW("output_norm.weight",1,H,1,1,0,&m->edge_resident_bytes);
    uint64_t maxexpert=0;
    for(uint32_t l=0;l<c.layers;l++) {
        uint64_t *cost=&m->memory[l].resident_bytes;
#define LW(s,r,x,y,z,mat) do {snprintf(name,sizeof name,"blk.%u.%s",l,s);GW(name,r,x,y,z,mat,cost);}while(0)
        LW("attn_norm.weight",1,H,1,1,0);LW("ffn_norm.weight",1,H,1,1,0);
        LW("attn_q_a.weight",2,H,c.query,1,1);LW("attn_q_a_norm.weight",1,c.query,1,1,0);
        LW("attn_q_b.weight",2,c.query,(uint64_t)c.heads*c.key,1,1);
        LW("attn_kv_a_mqa.weight",2,H,c.kv+c.rope,1,1);LW("attn_kv_a_norm.weight",1,c.kv,1,1,0);
        snprintf(name,sizeof name,"blk.%u.attn_k_b.weight",l);
        if(lmb_glmd_matrix(&v,name,c.key-c.rope,c.kv,c.heads,cost))goto done;
        snprintf(name,sizeof name,"blk.%u.attn_v_b.weight",l);
        if(lmb_glmd_matrix(&v,name,c.kv,c.value,c.heads,cost))goto done;
        LW("attn_output.weight",2,(uint64_t)c.heads*c.value,H,1,1);
        if(c.indexed[l]) {
            LW("indexer.attn_q_b.weight",2,c.query,(uint64_t)c.ih*c.idim,1,1);
            LW("indexer.attn_k.weight",2,H,c.idim,1,1);LW("indexer.proj.weight",2,H,c.ih,1,1);
            LW("indexer.k_norm.weight",1,c.idim,1,1,0);LW("indexer.k_norm.bias",1,c.idim,1,1,0);
        }
        uint32_t I=l<c.first?c.dense:c.inter*c.shared;
        LW(l<c.first?"ffn_gate.weight":"ffn_gate_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_up.weight":"ffn_up_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_down.weight":"ffn_down_shexp.weight",2,I,H,1,1);
        if(l>=c.first) {
            LW("ffn_gate_inp.weight",2,H,c.experts,1,0);LW("exp_probs_b.bias",1,c.experts,1,1,0);
            uint64_t before=*cost;
            if(packed) {
                snprintf(name,sizeof name,"blk.%u.ffn_exps_colibri",l);
                const LmbGgufTensor *t=lmb_gguf_tensor(g,name);uint64_t stride,total=0;
                if(!t || t->rank!=2 || t->type!=24 || lmb_gguf_inventory_item(ps,l,&stride) || !stride ||
                    t->shape[0]!=stride || t->shape[1]!=c.experts)goto done;
                v.seen[t-g->tensors]=1;
                for(unsigned k=0;k<3;k++) {
                    uint64_t type,bytes;
                    if(lmb_gguf_inventory_item(pt,3*l+k,&type) || lmb_gguf_inventory_item(pb,3*l+k,&bytes) ||
                       type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES || !LMB_GGUF_TYPES[type].decodable)goto done;
                    const LmbGgufType *q=&LMB_GGUF_TYPES[type];uint64_t cols=k==2?c.inter:H,rows=k==2?H:c.inter,x,y;
                    if(lmb_gguf_inventory_item(pd,6*l+2*k,&x) || lmb_gguf_inventory_item(pd,6*l+2*k+1,&y) || x!=cols || y!=rows ||
                        cols%q->block || bytes!=lmb_size_mul(cols/q->block,q->bytes*rows))goto done;
                    LmbGgufTensor weight={.type=(uint32_t)type,.rank=2,.shape={cols,rows}};
                    uint64_t one=lmb_gguf_held_bytes(&weight,1,1);
                    if(type==0 || type==1 || type==30)one=lmb_size_mul(lmb_size_mul(cols,rows),5);
                    *cost=lmb_size_add(*cost,lmb_size_mul(one,c.experts));total=lmb_size_add(total,bytes);
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
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i]) {
        /* NextN is not part of the text chain. Its tensors stay on the source. */
        uint64_t l;const char *tail=!strncmp(g->tensors[i].name,"blk.",4)?lmb_plan_uint(g->tensors[i].name+4,&l):NULL;
        if(!c.nextn || !tail || *tail!='.' || l<c.layers || l>=c.blocks)goto done;
    }
    uint64_t headers=lmb_size_add(lmb_size_mul(g->header_bytes,6),lmb_size_mul(g->ntensors,1024));
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,headers);
    m->edge_scratch_fixed_bytes=lmb_size_add(v.largest,((uint64_t)c.vocab+H)*4);
    m->edge_scratch_fixed_bytes=lmb_size_add(m->edge_scratch_fixed_bytes,width*512u*8u);
    m->segment_fixed_bytes=lmb_size_add(headers,(uint64_t)c.layers*(8192u+(uint64_t)c.experts*2048u));
    uint64_t wide=H+(uint64_t)c.heads*(c.key+c.value)+c.query+c.kv+c.dense+(uint64_t)c.shared*c.inter+
        (uint64_t)c.topk*(H+c.inter)+c.experts+(uint64_t)c.ih*c.idim;
    m->scratch_fixed_bytes=lmb_size_add(wide*512u*16u*4u,lmb_size_add(v.largest,lmb_size_mul(maxexpert,c.experts<256?c.experts:256)));
    m->scratch_fixed_bytes=lmb_size_add(m->scratch_fixed_bytes,lmb_size_mul(maxexpert,64));
    m->scratch_fixed_bytes=lmb_size_add(m->scratch_fixed_bytes,width*512u*12u+(uint64_t)c.itop*4u);
    m->scratch_token_bytes=256u*32u;m->heads=c.heads;m->kv_heads=c.heads;m->moe_intermediate=c.inter;
    m->max_context=c.context;m->memory_contract=1;rc=0;
done:free(v.seen);return rc;
#undef GW
}
#endif
