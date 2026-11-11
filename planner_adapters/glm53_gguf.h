#ifndef LMB_PLAN_GLM53_GGUF_H
#define LMB_PLAN_GLM53_GGUF_H
#include "gguf_inventory.h"

typedef struct {
    uint32_t blocks,layers,nextn,h,vocab,heads,experts,topk,inter,dense,shared,first;
    uint32_t query,kv,key,value,ih,idim,itop,pool,tail,kd,conv,hc,iters,context,norm;
    uint8_t full[120];
    double eps,heps,scale,clamp,gate;
} LmbGlm53Gguf;
static const LmbGgufValue *lmb_g5_value(const LmbGguf *g,const char *key) {
    char name[128];int n=snprintf(name,sizeof name,"glm5-next.%s",key);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_g5_uint(const LmbGguf *g,const char *key,uint32_t def,uint32_t lo,uint32_t hi,uint32_t *out) {
    const LmbGgufValue *v=lmb_g5_value(g,key);uint64_t n=def;
    if((v && lmb_gguf_integer(v,&n)) || n<lo || n>hi)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_g5_geometry(const LmbGguf *g,LmbGlm53Gguf *c) {
    memset(c,0,sizeof *c);const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"glm5-next"))return -1;
#define GU(k,f,d,lo,hi) if(lmb_g5_uint(g,k,d,lo,hi,&c->f))return -1
    GU("block_count",blocks,0,1,121);GU("nextn_predict_layers",nextn,0,0,1);
    char name[128];snprintf(name,sizeof name,"blk.%u.nextn.eh_proj.weight",c->blocks-1);
    if(!lmb_gguf_tensor(g,name))c->nextn=0;
    c->layers=c->blocks-c->nextn;if(!c->layers || c->layers>120)return -1;
    GU("embedding_length",h,0,32,65536);GU("attention.head_count",heads,0,1,1024);
    GU("expert_count",experts,0,1,4096);GU("expert_used_count",topk,0,1,64);
    GU("expert_feed_forward_length",inter,0,32,1048576);GU("feed_forward_length",dense,0,32,16777216);
    GU("expert_shared_count",shared,1,1,64);GU("leading_dense_block_count",first,0,0,c->layers);
    GU("attention.q_lora_rank",query,0,1,1048576);GU("attention.kv_lora_rank",kv,0,1,4096);
    GU("attention.key_length_mla",key,0,1,65536);GU("attention.value_length_mla",value,0,1,65536);
    GU("attention.indexer.head_count",ih,0,1,1024);GU("attention.indexer.key_length",idim,0,1,65536);
    GU("attention.indexer.top_k",itop,0,1,1048576);GU("attention.indexer.kpool",pool,1,2,64);
    GU("attention.indexer.kpool_select_tail",tail,1,0,1);
    GU("kda.head_dim",kd,0,1,512);GU("ssm.conv_kernel",conv,0,1,8);
    GU("hyper_connection.count",hc,0,1,8);GU("hyper_connection.sinkhorn_iterations",iters,0,0,1024);
    GU("context_length",context,1048576,1,1048576);GU("expert_weights_norm",norm,1,1,1);
#undef GU
    uint32_t rope,gate;
    if(c->inter%32 || c->topk>c->experts || (uint64_t)c->heads*c->kd>1048576 ||
        (uint64_t)c->shared*c->inter>(c->dense>c->inter?c->dense:c->inter) ||
        lmb_g5_uint(g,"rope.dimension_count",0,0,0,&rope) || lmb_g5_uint(g,"expert_gating_func",2,2,2,&gate) ||
        lmb_g5_uint(g,"expert_group_count",1,1,1,&gate) || lmb_g5_uint(g,"expert_group_used_count",1,1,1,&gate))return -1;
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens"),*kinds=lmb_g5_value(g,"attention.head_count_kv"),
        *index=lmb_g5_value(g,"attention.indexer.types");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || !tokens->count || tokens->count>(1u<<22) ||
        !kinds || kinds->type!=9 || kinds->count!=c->blocks || (index && (index->type!=9 || index->count!=c->blocks)))return -1;
    c->vocab=(uint32_t)tokens->count;
    if(lmb_g5_uint(g,"vocab_size",c->vocab,c->vocab,c->vocab,&gate))return -1;
    for(uint32_t i=0;i<c->layers;i++) {
        uint64_t value;
        if(lmb_gguf_inventory_item(kinds,i,&value) || value>1)return -1;
        c->full[i]=(uint8_t)value;
        /* The pinned GLM53 MLA kernel computes its own indexer per layer. */
        if(index && (lmb_gguf_inventory_item(index,i,&value) || value>1 || (c->full[i] && !value)))return -1;
    }
    if(lmb_gguf_number(lmb_g5_value(g,"attention.layer_norm_rms_epsilon"),1e-5,&c->eps) ||
        lmb_gguf_number(lmb_g5_value(g,"hyper_connection.epsilon"),1e-6,&c->heps) ||
        lmb_gguf_number(lmb_g5_value(g,"expert_weights_scale"),1,&c->scale) ||
        lmb_gguf_number(lmb_g5_value(g,"kda.gate_lower_bound"),-5,&c->gate))return -1;
    c->clamp=10;const LmbGgufValue *clamp=lmb_g5_value(g,"swiglu_clamp_exp");
    if(clamp) {
        unsigned width=lmb_gguf_scalar_bytes(clamp->element_type);
        if(clamp->type!=9 || !clamp->count || !width || clamp->bytes!=width*clamp->count)return -1;
        for(uint64_t i=0;i<clamp->count;i++) {
            LmbGgufValue item={.type=clamp->element_type,.bytes=width,.data=clamp->data+i*width};double x;
            if(lmb_gguf_number(&item,0,&x) || (i && (float)x!=(float)c->clamp))return -1;
            c->clamp=x;
        }
    }
    return c->eps>0 && c->eps<=FLT_MAX && c->heps>0 && c->heps<=FLT_MAX &&
        c->scale>0 && c->scale<=FLT_MAX && c->gate<0 && c->gate>=-FLT_MAX &&
        c->clamp>=0 && c->clamp<=FLT_MAX?0:-1;
}
static int lmb_g5_matches(const char *cfg,const LmbGlm53Gguf *c) {
    const char *keys[]={"num_hidden_layers","hidden_size","vocab_size","num_attention_heads","n_routed_experts",
        "num_experts_per_tok","moe_intermediate_size","intermediate_size","n_shared_experts","first_k_dense_replace",
        "q_lora_rank","kv_lora_rank","qk_rope_head_dim","qk_nope_head_dim","v_head_dim","index_n_heads",
        "index_head_dim","index_topk","index_kpool","hc_mult","hc_sinkhorn_iters","max_position_embeddings","num_nextn_predict_layers"};
    uint32_t values[]={c->layers,c->h,c->vocab,c->heads,c->experts,c->topk,c->inter,c->dense,c->shared,c->first,
        c->query,c->kv,0,c->key,c->value,c->ih,c->idim,c->itop,c->pool,c->hc,c->iters,c->context,c->nextn};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++){uint32_t n;if(lmb_plan_u32(cfg,keys[i],values[i],values[i],&n))return -1;}
    const char *linear=lmb_json_member(cfg,"linear_attn_config");uint32_t n;
    if(!linear || *linear!='{' || lmb_plan_u32(linear,"num_heads",c->heads,c->heads,&n) ||
        lmb_plan_u32(linear,"head_dim",c->kd,c->kd,&n) || lmb_plan_u32(linear,"short_conv_kernel_size",c->conv,c->conv,&n))return -1;
    const char *fk[]={"rms_norm_eps","hc_eps","routed_scaling_factor","swiglu_limit","gate_lower_bound"};
    double fv[]={c->eps,c->heps,c->scale,c->clamp,c->gate};
    for(unsigned i=0;i<5;i++){double x;if(lmb_plan_number(i==4?linear:cfg,fk[i],NAN,&x) || (float)x!=(float)fv[i])return -1;}
    uint8_t full[120],sparse[120];
    if(lmb_plan_kinds(cfg,"layer_types","linear_attention","deepseek_sparse_attention",full,c->layers) || memcmp(full,c->full,c->layers) ||
        lmb_plan_kinds(cfg,"mlp_layer_types","dense","sparse",sparse,c->layers))return -1;
    for(uint32_t i=0;i<c->layers;i++)if(sparse[i]!=(i>=c->first))return -1;
    char text[64];
    if(lmb_json_string(cfg,"model_type",text,sizeof text) || strcmp(text,"glm5_next_text") ||
        lmb_json_string(cfg,"scoring_func",text,sizeof text) || strcmp(text,"sigmoid"))return -1;
    const char *norm=lmb_json_member(cfg,"norm_topk_prob");
    if(!norm || strncmp(norm,"true",4))return -1;
    norm=lmb_plan_space(norm+4);if(*norm!=',' && *norm!='}')return -1;
    if(lmb_json_member(cfg,"indexer_types")) {
        uint8_t index[120];if(lmb_plan_kinds(cfg,"indexer_types","shared","full",index,c->layers))return -1;
        for(uint32_t i=0;i<c->layers;i++)if(c->full[i] && !index[i])return -1;
    }
    const char *lk[]={"kda_layers","full_attn_layers"};
    for(unsigned k=0;k<2;k++)if(lmb_json_member(linear,lk[k])) {
        uint8_t kind[120]={0};if(lmb_plan_indices(linear,lk[k],kind,c->layers,0))return -1;
        for(uint32_t i=0;i<c->layers;i++)if(kind[i]!=(k?c->full[i]:!c->full[i]))return -1;
    }
    const char *tail=lmb_json_member(cfg,"index_kpool_always_select_tail"),*expect=c->tail?"true":"false";
    if(!tail || strncmp(tail,expect,strlen(expect)))return -1;
    tail=lmb_plan_space(tail+strlen(expect));return *tail==',' || *tail=='}'?0:-1;
}

static int lmb_g5_packed_expert(LmbGgufInventory *v,const LmbGlm53Gguf *c,uint32_t l,unsigned k,uint64_t *cost) {
    const LmbGguf *g=v->g;char name[128];snprintf(name,sizeof name,"blk.%u.ffn_exps_colibri",l);
    const LmbGgufTensor *t=lmb_gguf_tensor(g,name);
    const LmbGgufValue *pt=lmb_gguf_get(g,"colibri.expert_types"),*pb=lmb_gguf_get(g,"colibri.expert_bytes"),
        *ps=lmb_gguf_get(g,"colibri.expert_stride"),*pd=lmb_gguf_get(g,"colibri.expert_dims");
    if(!pt || !pb || !ps || !pd || pt->type!=9 || pt->element_type!=4 || pt->count!=3u*c->layers ||
        pb->type!=9 || pb->element_type!=10 || pb->count!=3u*c->layers ||
        ps->type!=9 || ps->element_type!=10 || ps->count!=c->layers ||
        pd->type!=9 || pd->element_type!=10 || pd->count!=6u*c->layers)return -1;
    uint64_t stride,total=0,held=0;
    if(!t || t->rank!=2 || t->type!=24 || lmb_gguf_inventory_item(ps,l,&stride) || !stride ||
        t->shape[0]!=stride || t->shape[1]!=c->experts)return -1;
    for(unsigned j=0;j<3;j++) {
        uint64_t type,bytes,x,y,cols=j==2?c->inter:c->h,rows=j==2?c->h:c->inter;
        if(lmb_gguf_inventory_item(pt,3*l+j,&type) || lmb_gguf_inventory_item(pb,3*l+j,&bytes) ||
            type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES || !LMB_GGUF_TYPES[type].decodable ||
            lmb_gguf_inventory_item(pd,6*l+2*j,&x) || lmb_gguf_inventory_item(pd,6*l+2*j+1,&y) || x!=cols || y!=rows)return -1;
        const LmbGgufType *q=&LMB_GGUF_TYPES[type];
        if(cols%q->block || bytes!=lmb_size_mul(cols/q->block,q->bytes*rows))return -1;
        total=lmb_size_add(total,bytes);
        if(j==k) {
            LmbGgufTensor part={.type=(uint32_t)type,.rank=2,.shape={cols,rows}};
            held=lmb_gguf_held_bytes(&part,1,1);
            if(type==0 || type==1 || type==30)held=lmb_size_mul(lmb_size_mul(cols,rows),5);
        }
    }
    if(total>stride || held==UINT64_MAX)return -1;
    v->seen[t-g->tensors]=1;if(held>v->largest)v->largest=held;
    *cost=lmb_size_add(*cost,lmb_size_mul(held,c->experts));return *cost==UINT64_MAX?-1:0;
}

static int lmb_g5_gguf_memory(const LmbGguf *g,const char *cfg,LmbModelShape *m) {
    LmbGlm53Gguf c;if(lmb_g5_geometry(g,&c) || lmb_g5_matches(cfg,&c) || m->layers!=c.layers ||
        m->hidden!=c.h || m->vocab!=c.vocab || m->experts!=c.experts || m->experts_per_tok!=c.topk)return -1;
    const char *layout=lmb_gguf_text(lmb_gguf_get(g,"colibri.layout"));
    int packed=layout && !strcmp(layout,"expert-packed-v1");if(layout && !packed)return -1;
    LmbGgufInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    uint64_t H=c.h,P=(uint64_t)c.heads*c.kd,HC=c.hc,M=HC*(HC+2),largest_slot[3]={0},expert_cost[120]={0};
    char name[128];int rc=-1;
#define GW(n,r,x,y,z,mat,cost) if(lmb_gguf_inventory_weight(&v,n,r,x,y,z,mat,cost))goto done
    GW("token_embd.weight",2,H,c.vocab,1,0,&m->edge_resident_bytes);
    GW("output_norm.weight",1,H,1,1,0,&m->edge_resident_bytes);
    if(lmb_gguf_tensor(g,"output.weight")){GW("output.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);}
    for(uint32_t l=0;l<c.layers;l++) {
        uint64_t *cost=&m->memory[l].resident_bytes;
#define LW(s,r,x,y,z,mat) do{snprintf(name,sizeof name,"blk.%u.%s",l,s);GW(name,r,x,y,z,mat,cost);}while(0)
        LW("attn_norm.weight",1,H,1,1,0);LW("ffn_norm.weight",1,H,1,1,0);
        LW("hc_attn_fn.weight",2,HC*H,M,1,0);LW("hc_ffn_fn.weight",2,HC*H,M,1,0);
        LW("hc_attn_base.weight",1,M,1,1,0);LW("hc_ffn_base.weight",1,M,1,1,0);
        LW("hc_attn_scale.weight",1,3,1,1,0);LW("hc_ffn_scale.weight",1,3,1,1,0);
        if(c.full[l]) {
            LW("attn_q_a.weight",2,H,c.query,1,1);LW("attn_q_a_norm.weight",1,c.query,1,1,0);
            LW("attn_q_b.weight",2,c.query,(uint64_t)c.heads*c.key,1,1);
            LW("attn_kv_a_mqa.weight",2,H,c.kv,1,1);LW("attn_kv_a_norm.weight",1,c.kv,1,1,0);
            snprintf(name,sizeof name,"blk.%u.attn_k_b.weight",l);
            const LmbGgufTensor *kb=lmb_gguf_tensor(g,name);
            if(kb && kb->rank==2){GW(name,2,c.key,(uint64_t)c.kv*c.heads,1,1,cost);}
            else {GW(name,3,c.key,c.kv,c.heads,1,cost);}
            snprintf(name,sizeof name,"blk.%u.attn_v_b.weight",l);
            const LmbGgufTensor *vb=lmb_gguf_tensor(g,name);
            if(vb && vb->rank==2){GW(name,2,c.kv,(uint64_t)c.value*c.heads,1,1,cost);}
            else {GW(name,3,c.kv,c.value,c.heads,1,cost);}
            LW("attn_output.weight",2,(uint64_t)c.heads*c.value,H,1,1);
            LW("indexer.attn_q_b.weight",2,c.query,(uint64_t)c.ih*c.idim,1,1);
            LW("indexer.attn_k.weight",2,H,c.idim,1,1);LW("indexer.proj.weight",2,H,c.ih,1,1);
            LW("indexer.k_norm.weight",1,c.idim,1,1,0);LW("indexer.k_norm.bias",1,c.idim,1,1,0);
            LW("indexer_compressor_ape.weight",2,c.idim,c.pool,1,0);
            LW("indexer_compressor_gate.weight",2,H,c.idim,1,1);
            m->session_token_bytes=lmb_size_add(m->session_token_bytes,((uint64_t)c.kv+2*c.idim)*4u);
        } else {
            LW("attn_q.weight",2,H,P,1,1);LW("attn_k.weight",2,H,P,1,1);LW("attn_v.weight",2,H,P,1,1);
            LW("attn_output.weight",2,P,H,1,1);
            LW("ssm_g_a.weight",2,H,c.kd,1,1);LW("ssm_g_b.weight",2,c.kd,P,1,1);
            LW("ssm_f_a.weight",2,H,c.kd,1,1);LW("ssm_f_b.weight",2,c.kd,P,1,1);
            LW("ssm_beta.weight",2,H,c.heads,1,1);LW("ssm_dt.bias",1,P,1,1,0);
            LW("ssm_a",1,c.heads,1,1,0);*cost=lmb_size_add(*cost,(uint64_t)c.heads*4u);
            LW("ssm_norm.weight",1,c.kd,1,1,0);
            LW("ssm_conv1d_q.weight",2,c.conv,P,1,0);LW("ssm_conv1d_k.weight",2,c.conv,P,1,0);LW("ssm_conv1d_v.weight",2,c.conv,P,1,0);
            m->session_fixed_bytes=lmb_size_add(m->session_fixed_bytes,(P*c.kd+3*P*c.conv)*4u);
        }
        uint32_t I=l<c.first?c.dense:c.inter*c.shared;
        LW(l<c.first?"ffn_gate.weight":"ffn_gate_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_up.weight":"ffn_up_shexp.weight",2,H,I,1,1);
        LW(l<c.first?"ffn_down.weight":"ffn_down_shexp.weight",2,I,H,1,1);
        if(l>=c.first) {
            LW("ffn_gate_inp.weight",2,H,c.experts,1,0);
            snprintf(name,sizeof name,"blk.%u.exp_probs_b.bias",l);
            if(lmb_gguf_tensor(g,name)){GW(name,1,c.experts,1,1,0,cost);}
            const char *suffix[]={"ffn_gate_exps.weight","ffn_up_exps.weight","ffn_down_exps.weight"};
            for(unsigned k=0;k<3;k++) {
                uint64_t before=*cost;
                if(packed){if(lmb_g5_packed_expert(&v,&c,l,k,cost))goto done;}
                else {LW(suffix[k],3,k==2?c.inter:H,k==2?H:c.inter,c.experts,1);}
                uint64_t one=(*cost-before)/c.experts;
                if(one>largest_slot[k])largest_slot[k]=one;
                expert_cost[l]+=*cost-before;
            }
        }
#undef LW
    }
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i]) {
        uint64_t l;const char *tail=!strncmp(g->tensors[i].name,"blk.",4)?lmb_plan_uint(g->tensors[i].name+4,&l):NULL;
        if(!c.nextn || !tail || *tail!='.' || l<c.layers || l>=c.blocks)goto done;
    }
    /* Native expert slots use the largest piece of every assigned layer.
     * A model-wide bound is safe for every possible contiguous placement. */
    uint64_t slot=largest_slot[0]+largest_slot[1]+largest_slot[2];
    for(uint32_t l=c.first;l<c.layers;l++)m->memory[l].resident_bytes=
        lmb_size_add(m->memory[l].resident_bytes-expert_cost[l],slot*c.experts);
    uint64_t headers=lmb_size_add(lmb_size_mul(g->header_bytes,6),lmb_size_mul(g->ntensors,1024));
    m->boundary_width=c.h*c.hc;m->max_context=c.context;
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,headers);
    m->edge_scratch_fixed_bytes=lmb_size_add(v.largest,((uint64_t)c.vocab+m->boundary_width)*4096u);
    m->session_fixed_bytes=lmb_size_add(m->session_fixed_bytes,(3*P+c.kd)*4u+(uint64_t)c.layers*1024);
    uint64_t wide=m->boundary_width+(uint64_t)c.heads*(c.key+c.kv+c.value)+c.ih*(uint64_t)c.idim+
        c.query+c.kv+c.h+P+c.dense+(uint64_t)c.shared*c.inter+c.experts+c.topk+(uint64_t)c.itop*c.pool+c.pool;
    m->scratch_token_bytes=lmb_size_mul(wide,64);
    m->scratch_fixed_bytes=lmb_size_add(v.largest,lmb_size_add(slot*64u,wide*64u));
    m->segment_fixed_bytes=lmb_size_add(headers,(uint64_t)c.layers*(8192u+(uint64_t)c.experts*512u));
    m->memory_contract=1;rc=0;
done:free(v.seen);return rc;
#undef GW
}
#endif
