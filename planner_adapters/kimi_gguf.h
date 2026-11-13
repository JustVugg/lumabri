#ifndef LMB_PLAN_KIMI_GGUF_H
#define LMB_PLAN_KIMI_GGUF_H
#include "gguf_inventory.h"

typedef struct {
    uint32_t layers,h,vocab,heads,query,kv,nope,rope,value,experts,topk,inter,latent,dense,shared,first,block,kd,conv,context;
    uint8_t kda[128];
    double eps,theta,beta,linear_beta,gate;
} LmbKimiGguf;
static const LmbGgufValue *lmb_k3_value(const LmbGguf *g,const char *key) {
    char name[128];int n=snprintf(name,sizeof name,"kimi-k3.%s",key);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_k3_uint(const LmbGguf *g,const char *key,uint32_t def,uint32_t lo,uint32_t hi,uint32_t *out) {
    const LmbGgufValue *v=lmb_k3_value(g,key);uint64_t n=def;
    if((v && lmb_gguf_integer(v,&n)) || n<lo || n>hi)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_k3_geometry(const LmbGguf *g,LmbKimiGguf *c) {
    memset(c,0,sizeof *c);const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"kimi-k3"))return -1;
#define KU(k,f,d,lo,hi) if(lmb_k3_uint(g,k,d,lo,hi,&c->f))return -1
    KU("block_count",layers,0,1,128);KU("embedding_length",h,0,32,65536);
    KU("attention.head_count",heads,0,1,1024);KU("attention.q_lora_rank",query,0,1,65536);
    KU("attention.kv_lora_rank",kv,0,1,4096);KU("rope.dimension_count",rope,0,2,256);
    KU("attention.key_length_mla",nope,0,1,65536);KU("attention.value_length_mla",value,0,1,65536);
    KU("expert_count",experts,0,1,4096);KU("expert_used_count",topk,0,1,64);
    KU("expert_feed_forward_length",inter,0,32,1048576);KU("expert_latent_length",latent,0,32,1048576);
    KU("feed_forward_length",dense,0,1,1048576);KU("expert_shared_count",shared,0,1,64);
    KU("leading_dense_block_count",first,0,0,c->layers);KU("attn_res.block_size",block,0,1,c->layers);
    KU("kda.head_dim",kd,0,1,512);KU("ssm.conv_kernel",conv,0,1,8);KU("context_length",context,1048576,1,1048576);
#undef KU
    if(c->nope<=c->rope || c->rope%2 || c->latent%32 || c->inter%32 || c->topk>c->experts ||
        (c->layers+c->block-1)/c->block+1>16 || (uint64_t)c->heads*c->kd>(1u<<20))return -1;
    c->nope-=c->rope;
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens"),*kinds=lmb_k3_value(g,"attention.head_count_kv");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || !tokens->count || tokens->count>(1u<<22) ||
        !kinds || kinds->type!=9 || kinds->count!=c->layers)return -1;
    c->vocab=(uint32_t)tokens->count;
    uint32_t control;
    if(lmb_k3_uint(g,"vocab_size",c->vocab,c->vocab,c->vocab,&control) ||
        lmb_k3_uint(g,"expert_gating_func",2,2,2,&control) ||
        lmb_k3_uint(g,"expert_weights_norm",1,1,1,&control) ||
        lmb_k3_uint(g,"expert_group_count",1,1,1,&control) ||
        lmb_k3_uint(g,"expert_group_used_count",1,1,1,&control))return -1;
    double scale;
    if(lmb_gguf_number(lmb_k3_value(g,"expert_weights_scale"),1,&scale) || scale!=1)return -1;
    for(uint32_t i=0;i<c->layers;i++) {uint64_t n;if(lmb_gguf_inventory_item(kinds,i,&n) || n>1)return -1;c->kda[i]=!n;}
    if(lmb_gguf_number(lmb_k3_value(g,"attention.layer_norm_rms_epsilon"),1e-5,&c->eps) ||
        lmb_gguf_number(lmb_k3_value(g,"rope.freq_base"),10000,&c->theta) ||
        lmb_gguf_number(lmb_k3_value(g,"activation.situ_beta"),1,&c->beta) ||
        lmb_gguf_number(lmb_k3_value(g,"activation.situ_linear_beta"),1,&c->linear_beta) ||
        lmb_gguf_number(lmb_k3_value(g,"kda.gate_lower_bound"),-5,&c->gate))return -1;
    return c->eps>0 && c->eps<=FLT_MAX && c->theta>0 && c->theta<=FLT_MAX &&
        c->beta>0 && c->beta<=FLT_MAX && c->linear_beta>0 && c->linear_beta<=FLT_MAX &&
        c->gate<0 && c->gate>=-FLT_MAX?0:-1;
}
static int lmb_k3_matches(const char *cfg,const LmbKimiGguf *c) {
    const char *keys[]={"num_hidden_layers","hidden_size","vocab_size","num_attention_heads","q_lora_rank","kv_lora_rank",
        "qk_nope_head_dim","qk_rope_head_dim","v_head_dim","num_experts","num_experts_per_token","moe_intermediate_size",
        "routed_expert_hidden_size","intermediate_size","num_shared_experts","first_k_dense_replace","attn_res_block_size","max_position_embeddings"};
    uint32_t values[]={c->layers,c->h,c->vocab,c->heads,c->query,c->kv,c->nope,c->rope,c->value,c->experts,c->topk,c->inter,
        c->latent,c->dense,c->shared,c->first,c->block,c->context};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++){uint32_t n;if(lmb_plan_u32(cfg,keys[i],values[i],values[i],&n))return -1;}
    const char *linear=lmb_json_member(cfg,"linear_attn_config"),*rp=lmb_json_member(cfg,"rope_parameters");uint32_t n;
    if(!linear || *linear!='{' || !rp || *rp!='{' || lmb_plan_u32(linear,"num_heads",c->heads,c->heads,&n) ||
        lmb_plan_u32(linear,"head_dim",c->kd,c->kd,&n) || lmb_plan_u32(linear,"short_conv_kernel_size",c->conv,c->conv,&n))return -1;
    uint8_t kda[128];if(lmb_plan_indices(linear,"kda_layers",kda,c->layers,1) || memcmp(kda,c->kda,c->layers))return -1;
    const char *keysf[]={"rms_norm_eps","activation_situ_beta","activation_situ_linear_beta","gate_lower_bound","rope_theta"};
    double vals[]={c->eps,c->beta,c->linear_beta,c->gate,c->theta};
    for(unsigned i=0;i<5;i++){double x;if(lmb_plan_number(i==3?linear:i==4?rp:cfg,keysf[i],NAN,&x) || (float)x!=(float)vals[i])return -1;}
    if(lmb_plan_optional(cfg,"index_hd",0,0,0,&n))return -1;
    char type[64];return lmb_json_string(cfg,"model_type",type,sizeof type) || strcmp(type,"kimi_linear")?-1:0;
}

/* Kimi keeps expert matrices in the file's block types, with one padded slot
 * sized to the largest assigned layer. Float expert matrices are not accepted
 * by the pinned engine's GGUF expert table. */
static int lmb_k3_experts(LmbGgufInventory *v,const LmbKimiGguf *c,uint32_t layer,uint64_t *slot) {
    char name[128];snprintf(name,sizeof name,"blk.%u.ffn_exps_colibri",layer);
    const LmbGgufTensor *packed=lmb_gguf_tensor(v->g,name);
    const char *layout=lmb_gguf_text(lmb_gguf_get(v->g,"colibri.layout"));
    if(layout && strcmp(layout,"expert-packed-v1"))return -1;
    const LmbGgufValue *pt=NULL,*pb=NULL,*ps=NULL,*pd=NULL;uint64_t stride=0,total=0;
    if(layout) {
        pt=lmb_gguf_get(v->g,"colibri.expert_types");pb=lmb_gguf_get(v->g,"colibri.expert_bytes");
        ps=lmb_gguf_get(v->g,"colibri.expert_stride");pd=lmb_gguf_get(v->g,"colibri.expert_dims");
        if(!packed || packed->rank!=2 || packed->type!=24 || !pt || !pb || !ps || !pd ||
            pt->element_type!=4 || pb->element_type!=10 || ps->element_type!=10 || pd->element_type!=10 ||
            pt->count!=3u*c->layers || pb->count!=3u*c->layers || ps->count!=c->layers || pd->count!=6u*c->layers ||
            lmb_gguf_inventory_item(ps,layer,&stride) || packed->shape[0]!=stride || packed->shape[1]!=c->experts)return -1;
    } else if(packed)return -1;
    const char *suffix[]={"gate","up","down"};
    for(unsigned k=0;k<3;k++) {
        uint64_t cols=k==2?c->inter:c->latent,rows=k==2?c->latent:c->inter,type,bytes;
        if(layout) {
            uint64_t x,y;
            if(lmb_gguf_inventory_item(pt,3*layer+k,&type) || lmb_gguf_inventory_item(pb,3*layer+k,&bytes) ||
                lmb_gguf_inventory_item(pd,6*layer+2*k,&x) || lmb_gguf_inventory_item(pd,6*layer+2*k+1,&y) || x!=cols || y!=rows)return -1;
        } else {
            snprintf(name,sizeof name,"blk.%u.ffn_%s_exps.weight",layer,suffix[k]);const LmbGgufTensor *t=lmb_gguf_tensor(v->g,name);
            if(!t || t->rank!=3 || t->shape[0]!=cols || t->shape[1]!=rows || t->shape[2]!=c->experts)return -1;
            type=t->type;bytes=t->bytes/c->experts;v->seen[t-v->g->tensors]=1;
        }
        if(type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES || type==0 || type==1 || type==30)return -1;
        const LmbGgufType *q=&LMB_GGUF_TYPES[type];
        if(!q->decodable || cols%q->block || bytes!=lmb_size_mul(cols/q->block,lmb_size_mul(q->bytes,rows)))return -1;
        total=lmb_size_add(total,bytes);
    }
    if(total==UINT64_MAX || (layout && total>stride))return -1;
    if(layout)v->seen[packed-v->g->tensors]=1;
    *slot=total;return 0;
}
static int lmb_k3_gguf_memory(const LmbGguf *g,const char *cfg,LmbModelShape *m) {
    LmbKimiGguf c;if(lmb_k3_geometry(g,&c) || lmb_k3_matches(cfg,&c) || m->hidden!=c.h || m->layers!=c.layers || m->vocab!=c.vocab)return -1;
    LmbGgufInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    uint64_t H=c.h,P=(uint64_t)c.heads*c.kd,max_slot=0;char name[128];int rc=-1;
#define KW(n,r,x,y,z,mat,cost) if(lmb_gguf_inventory_weight(&v,n,r,x,y,z,mat,cost))goto done
    KW("token_embd.weight",2,H,c.vocab,1,0,&m->edge_resident_bytes);
    KW("output.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    KW("output_norm.weight",1,H,1,1,0,&m->edge_resident_bytes);
    KW("output_res_score.weight",1,H,1,1,0,&m->edge_resident_bytes);
    for(uint32_t l=0;l<c.layers;l++) {
        uint64_t *cost=&m->memory[l].resident_bytes;
#define LW(s,r,x,y,z,mat) do{snprintf(name,sizeof name,"blk.%u.%s",l,s);KW(name,r,x,y,z,mat,cost);}while(0)
        LW("attn_norm.weight",1,H,1,1,0);LW("ffn_norm.weight",1,H,1,1,0);
        LW("attn_res_score.weight",1,H,1,1,0);LW("ffn_res_score.weight",1,H,1,1,0);
        if(c.kda[l]) {
            LW("attn_q.weight",2,H,P,1,1);LW("attn_k.weight",2,H,P,1,1);LW("attn_v.weight",2,H,P,1,1);
            LW("ssm_g.weight",2,H,P,1,1);LW("attn_output.weight",2,P,H,1,1);
            LW("ssm_f_a.weight",2,H,c.kd,1,0);LW("ssm_f_b.weight",2,c.kd,P,1,0);LW("ssm_beta.weight",2,H,c.heads,1,0);
            LW("ssm_dt.bias",1,P,1,1,0);LW("ssm_norm.weight",1,c.kd,1,1,0);LW("ssm_a",1,c.heads,1,1,0);
            LW("ssm_conv1d_q.weight",2,c.conv,P,1,0);LW("ssm_conv1d_k.weight",2,c.conv,P,1,0);LW("ssm_conv1d_v.weight",2,c.conv,P,1,0);
            m->memory[l].state_fixed_bytes=(P*c.kd+3*P*c.conv)*4;
        } else {
            LW("attn_q_a.weight",2,H,c.query,1,1);LW("attn_q_a_norm.weight",1,c.query,1,1,0);
            LW("attn_q_b.weight",2,c.query,(uint64_t)c.heads*(c.nope+c.rope),1,1);
            LW("attn_kv_a_mqa.weight",2,H,c.kv+c.rope,1,1);LW("attn_kv_a_norm.weight",1,c.kv,1,1,0);
            /* MLA split halves are read as floats then recombined and quantized. */
            uint64_t halves=*cost;
            LW("attn_k_b.weight",3,c.nope,c.kv,c.heads,0);LW("attn_v_b.weight",3,c.kv,c.value,c.heads,0);
            *cost=lmb_size_add(halves,(uint64_t)c.heads*(c.nope+c.value)*c.kv*5);
            LW("attn_output.weight",2,(uint64_t)c.heads*c.value,H,1,1);LW("attn_gate.weight",2,H,(uint64_t)c.heads*c.value,1,1);
            m->memory[l].state_token_bytes=(c.kv+c.rope)*4u;
        }
        uint32_t I=l<c.first?c.dense:c.inter*c.shared;
        LW(l<c.first?"ffn_gate.weight":"ffn_gate_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_up.weight":"ffn_up_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_down.weight":"ffn_down_shexp.weight",2,I,H,1,1);
        if(l>=c.first) {
            LW("ffn_gate_inp.weight",2,H,c.experts,1,0);LW("exp_probs_b.bias",1,c.experts,1,1,0);
            LW("ffn_routed_norm.weight",1,c.latent,1,1,0);LW("ffn_routed_down.weight",2,H,c.latent,1,1);LW("ffn_routed_up.weight",2,c.latent,H,1,1);
            uint64_t slot;if(lmb_k3_experts(&v,&c,l,&slot))goto done;if(slot>max_slot)max_slot=slot;
        }
#undef LW
    }
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i])goto done;
    for(uint32_t l=c.first;l<c.layers;l++)m->memory[l].resident_bytes=lmb_size_add(m->memory[l].resident_bytes,(max_slot+8192u)*c.experts);
    m->boundary_width=H*(1+(c.layers+c.block-1)/c.block);m->max_context=c.context;
    uint64_t index=lmb_size_add(g->header_bytes*6u,((uint64_t)g->ntensors+c.layers*c.experts*3u)*1024u);
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,index);
    m->edge_scratch_fixed_bytes=lmb_size_add(v.largest,(H*1024u+m->boundary_width*128u+c.vocab)*4u);
    uint64_t wide=m->boundary_width+(uint64_t)c.heads*(c.nope+c.rope+2u*c.value)+P*(c.kd+8u)+
        (uint64_t)c.shared*c.inter+c.dense+c.latent+c.query+c.kv+c.experts+(uint64_t)c.topk*(c.latent+c.inter);
    m->scratch_fixed_bytes=lmb_size_add(v.largest,wide*(128u*16u+1024u)*4u);
    m->scratch_token_bytes=256u*32u+(uint64_t)c.layers*(c.kv+c.rope)*4u;
    m->segment_fixed_bytes=lmb_size_add(index,(uint64_t)c.layers*(4096u+c.experts*512u));
    m->experts=c.experts;m->experts_per_tok=c.topk;m->moe_intermediate=c.inter;m->memory_contract=1;rc=0;
done:free(v.seen);return rc;
#undef KW
}
#endif
