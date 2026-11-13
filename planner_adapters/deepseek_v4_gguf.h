#ifndef LMB_PLAN_DEEPSEEK_V4_GGUF_H
#define LMB_PLAN_DEEPSEEK_V4_GGUF_H
#include "gguf_inventory.h"
typedef struct {
    uint32_t layers,h,vocab,heads,hd,qr,rope,og,orr,window,ih,id,topk,e,k,inter,hash,hc,iters,context,orig,fast,slow;
    uint32_t ratios[LMB_PLAN_LAYER_MAX];
    double eps,heps,scale,clamp,theta,ctheta,factor;
    int normalized;
} LmbV4Gguf;
static const LmbGgufValue *lmb_v4g_value(const LmbGguf *g,const char *key) {
    char name[128];int n=snprintf(name,sizeof name,"deepseek4.%s",key);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_v4g_uint(const LmbGguf *g,const char *key,uint32_t def,uint32_t lo,uint32_t hi,uint32_t *out) {
    double n;if(lmb_gguf_number(lmb_v4g_value(g,key),def,&n) || n<lo || n>hi || floor(n)!=n)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_v4g_geometry(const LmbGguf *g,LmbV4Gguf *c) {
    memset(c,0,sizeof *c);const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"deepseek4"))return -1;
    uint32_t declared,shared,gating,kv;
#define VI(key,field,def,lo,hi) if(lmb_v4g_uint(g,key,def,lo,hi,&c->field))return -1
    VI("embedding_length",h,0,128,65536);VI("attention.head_count",heads,0,1,1024);
    VI("attention.key_length",hd,0,2,65536);VI("attention.q_lora_rank",qr,0,128,65536);
    VI("rope.dimension_count",rope,0,2,65536);VI("attention.output_group_count",og,0,1,c->heads);
    VI("attention.output_lora_rank",orr,0,1,65536);VI("attention.sliding_window",window,0,1,1048576);
    VI("attention.indexer.head_count",ih,0,1,1024);VI("attention.indexer.key_length",id,0,2,65536);
    VI("attention.indexer.top_k",topk,0,1,1048576);VI("expert_count",e,0,1,4096);
    VI("expert_used_count",k,0,1,256);VI("expert_feed_forward_length",inter,0,128,1048576);
    VI("hyper_connection.count",hc,0,1,16);VI("hyper_connection.sinkhorn_iterations",iters,0,1,1024);
    VI("context_length",context,1048576,4,1048576);
    VI("rope.scaling.original_context_length",orig,c->context,1,1048576);
    VI("rope.scaling.yarn_beta_fast",fast,32,1,INT32_MAX);VI("rope.scaling.yarn_beta_slow",slow,1,1,INT32_MAX);
#undef VI
    if(lmb_v4g_uint(g,"block_count",0,1,LMB_PLAN_LAYER_MAX,&declared) ||
       lmb_v4g_uint(g,"expert_shared_count",1,1,1,&shared) ||
       lmb_v4g_uint(g,"expert_gating_func",4,4,4,&gating) ||
       lmb_v4g_uint(g,"attention.head_count_kv",1,1,1,&kv))return -1;
    for(uint32_t i=0;i<declared;i++) {
        char name[96];snprintf(name,sizeof name,"blk.%u.attn_q_a.weight",i);
        if(!lmb_gguf_tensor(g,name))break;
        c->layers++;
    }
    if(!c->layers || lmb_v4g_uint(g,"hash_layer_count",0,0,c->layers,&c->hash) ||
       c->h%128 || c->inter%128 || c->qr%128 || c->heads%c->og || c->k>c->e ||
       c->rope%2 || c->rope>c->hd || c->rope>c->id ||
       ((uint64_t)(c->heads/c->og)*c->hd)%128 || ((uint64_t)c->og*c->orr)%128 || c->fast<c->slow)return -1;
    const LmbGgufTensor *emb=lmb_gguf_tensor(g,"token_embd.weight");
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!emb || emb->rank!=2 || emb->shape[0]!=c->h || !emb->shape[1] || emb->shape[1]>1048576 ||
       !tokens || tokens->type!=9 || tokens->element_type!=8 || tokens->count!=emb->shape[1])return -1;
    c->vocab=(uint32_t)emb->shape[1];
    const LmbGgufValue *r=lmb_v4g_value(g,"attention.compress_ratios");
    if(!r || r->type!=9 || r->count<c->layers || r->count>LMB_PLAN_LAYER_MAX+16)return -1;
    for(uint32_t i=0;i<c->layers;i++) {
        uint64_t value;if(lmb_gguf_inventory_item(r,i,&value) || (value!=0 && value!=4 && value!=8 && value!=128))return -1;
        c->ratios[i]=(uint32_t)value;
    }
    uint64_t normalized=1;const LmbGgufValue *nv=lmb_v4g_value(g,"expert_weights_norm");
    if(nv && (lmb_gguf_integer(nv,&normalized) || normalized>1))return -1;
    c->normalized=(int)normalized;
#define VF(key,field,def) if(lmb_gguf_number(lmb_v4g_value(g,key),def,&c->field) || c->field<=0 || c->field>FLT_MAX)return -1
    VF("attention.layer_norm_rms_epsilon",eps,1e-6);VF("hyper_connection.epsilon",heps,1e-6);
    VF("expert_weights_scale",scale,1);VF("rope.freq_base",theta,10000);
    VF("attention.compress_rope_freq_base",ctheta,NAN);VF("rope.scaling.factor",factor,1);
#undef VF
    const LmbGgufValue *clamp=lmb_v4g_value(g,"swiglu_clamp_exp");c->clamp=10;
    if(clamp) {
        if(clamp->type!=9 || (clamp->count!=1 && clamp->count<c->layers))return -1;
        unsigned bytes=lmb_gguf_scalar_bytes(clamp->element_type);
        if(!bytes || clamp->bytes/bytes!=clamp->count)return -1;
        for(uint32_t i=0;i<c->layers && i<clamp->count;i++) {
            LmbGgufValue v={.type=clamp->element_type,.bytes=bytes,.data=clamp->data+i*bytes};double n;
            if(lmb_gguf_number(&v,NAN,&n) || n<0 || n>FLT_MAX || (i && n!=c->clamp))return -1;
            c->clamp=n;
        }
    }
    const LmbGgufValue *rt=lmb_v4g_value(g,"rope.scaling.type");
    if(rt && (!lmb_gguf_text(rt) || strcmp(lmb_gguf_text(rt),"yarn")))return -1;
    return 0;
}
static int lmb_v4g_matches(const char *cfg,const LmbV4Gguf *c) {
    const char *keys[]={"num_hidden_layers","hidden_size","vocab_size","num_attention_heads","head_dim","q_lora_rank",
        "qk_rope_head_dim","o_groups","o_lora_rank","sliding_window","index_n_heads","index_head_dim","index_topk",
        "n_routed_experts","num_experts_per_tok","moe_intermediate_size","num_hash_layers","hc_mult","hc_sinkhorn_iters",
        "max_position_embeddings","n_shared_experts","num_nextn_predict_layers"};
    uint32_t values[]={c->layers,c->h,c->vocab,c->heads,c->hd,c->qr,c->rope,c->og,c->orr,c->window,c->ih,c->id,c->topk,
        c->e,c->k,c->inter,c->hash,c->hc,c->iters,c->context,1,0};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++){uint32_t v;if(lmb_plan_u32(cfg,keys[i],values[i],values[i],&v))return -1;}
    const char *reals[]={"rms_norm_eps","hc_eps","routed_scaling_factor","swiglu_limit","rope_theta","compress_rope_theta"};
    double rv[]={c->eps,c->heps,c->scale,c->clamp,c->theta,c->ctheta};
    for(unsigned i=0;i<sizeof rv/sizeof *rv;i++){double v;if(lmb_plan_number(cfg,reals[i],NAN,&v) || (float)v!=(float)rv[i])return -1;}
    uint64_t ratios[LMB_PLAN_LAYER_MAX];unsigned nr;
    if(lmb_plan_array(lmb_json_member(cfg,"compress_ratios"),ratios,LMB_PLAN_LAYER_MAX,&nr) || nr!=c->layers)return -1;
    for(unsigned i=0;i<nr;i++)if(ratios[i]!=c->ratios[i])return -1;
    const char *rp=lmb_json_member(cfg,"rope_scaling"),*norm=lmb_json_member(cfg,"norm_topk_prob");
    uint32_t v;double factor;
    if(!norm || strncmp(norm,c->normalized?"true":"false",c->normalized?4:5) || !rp || *rp!='{' ||
       lmb_plan_u32(rp,"original_max_position_embeddings",c->orig,c->orig,&v) ||
       lmb_plan_u32(rp,"beta_fast",c->fast,c->fast,&v) || lmb_plan_u32(rp,"beta_slow",c->slow,c->slow,&v) ||
       lmb_plan_number(rp,"factor",NAN,&factor) || (float)factor!=(float)c->factor ||
       lmb_v4_string(cfg,"expert_dtype","fp4") || lmb_v4_string(cfg,"scoring_func","sqrtsoftplus") ||
       lmb_v4_string(cfg,"topk_method","noaux_tc"))return -1;
    return 0;
}
/* Names follow the pinned st_gguf.h V4 loader; bytes remain in GGUF. */
static const char *lmb_v4g_name(const char *hf) {
    static const struct {const char *hf,*gg;} names[]={
        {"attn.attn_sink","attn_sinks.weight"},{"attn.q_norm.weight","attn_q_a_norm.weight"},{"attn.kv_norm.weight","attn_kv_a_norm.weight"},
        {"attn.wq_a.weight","attn_q_a.weight"},{"attn.wq_b.weight","attn_q_b.weight"},{"attn.wkv.weight","attn_kv.weight"},
        {"attn.wo_a.weight","attn_output_a.weight"},{"attn.wo_b.weight","attn_output_b.weight"},
        {"attn.compressor.ape","attn_compressor_ape.weight"},{"attn.compressor.norm.weight","attn_compressor_norm.weight"},
        {"attn.compressor.wkv.weight","attn_compressor_kv.weight"},{"attn.compressor.wgate.weight","attn_compressor_gate.weight"},
        {"attn.indexer.compressor.ape","indexer_compressor_ape.weight"},{"attn.indexer.compressor.norm.weight","indexer_compressor_norm.weight"},
        {"attn.indexer.compressor.wkv.weight","indexer_compressor_kv.weight"},{"attn.indexer.compressor.wgate.weight","indexer_compressor_gate.weight"},
        {"attn.indexer.weights_proj.weight","indexer.proj.weight"},{"attn.indexer.wq_b.weight","indexer.attn_q_b.weight"},
        {"ffn.gate.weight","ffn_gate_inp.weight"},{"ffn.gate.bias","exp_probs_b.bias"},{"ffn.gate.tid2eid","ffn_gate_tid2eid.weight"},
        {"ffn.shared_experts.w1.weight","ffn_gate_shexp.weight"},{"ffn.shared_experts.w2.weight","ffn_down_shexp.weight"},
        {"ffn.shared_experts.w3.weight","ffn_up_shexp.weight"},{"attn_norm.weight","attn_norm.weight"},{"ffn_norm.weight","ffn_norm.weight"},
        {"hc_attn_base","hc_attn_base.weight"},{"hc_attn_fn","hc_attn_fn.weight"},{"hc_attn_scale","hc_attn_scale.weight"},
        {"hc_ffn_base","hc_ffn_base.weight"},{"hc_ffn_fn","hc_ffn_fn.weight"},{"hc_ffn_scale","hc_ffn_scale.weight"}};
    for(unsigned i=0;i<sizeof names/sizeof *names;i++)if(!strcmp(names[i].hf,hf))return names[i].gg;
    return NULL;
}
static int lmb_v4g_experts(LmbGgufInventory *v,const LmbV4Gguf *c,uint32_t layer,uint64_t *slot) {
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
            lmb_gguf_inventory_item(ps,layer,&stride) || packed->shape[0]!=stride || packed->shape[1]!=c->e)return -1;
    } else if(packed)return -1;
    const char *suffix[]={"gate","up","down"};
    for(unsigned k=0;k<3;k++) {
        uint64_t cols=k==2?c->inter:c->h,rows=k==2?c->h:c->inter,type,bytes;
        if(layout) {
            uint64_t x,y;
            if(lmb_gguf_inventory_item(pt,3*layer+k,&type) || lmb_gguf_inventory_item(pb,3*layer+k,&bytes) ||
                lmb_gguf_inventory_item(pd,6*layer+2*k,&x) || lmb_gguf_inventory_item(pd,6*layer+2*k+1,&y) || x!=cols || y!=rows)return -1;
        } else {
            snprintf(name,sizeof name,"blk.%u.ffn_%s_exps.weight",layer,suffix[k]);const LmbGgufTensor *t=lmb_gguf_tensor(v->g,name);
            if(!t || t->rank!=3 || t->shape[0]!=cols || t->shape[1]!=rows || t->shape[2]!=c->e)return -1;
            type=t->type;bytes=t->bytes/c->e;v->seen[t-v->g->tensors]=1;
        }
        if(type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES || type==0 || type==1 || type==30)return -1;
        const LmbGgufType *q=&LMB_GGUF_TYPES[type];
        if(!q->decodable || cols%q->block || bytes!=lmb_size_mul(cols/q->block,lmb_size_mul(q->bytes,rows)))return -1;
        total=lmb_size_add(total,bytes);
    }
    if(total==UINT64_MAX || (layout && total>stride))return -1;
    if(layout)v->seen[packed-v->g->tensors]=1;
    if(total>v->largest)v->largest=total;
    *slot=total;return 0;
}
static int lmb_v4g_memory(const LmbGguf *g,const char *cfg,LmbModelShape *m) {
    LmbV4Gguf c;if(lmb_v4g_geometry(g,&c) || lmb_v4g_matches(cfg,&c) || m->layers!=c.layers || m->hidden!=c.h || m->vocab!=c.vocab)return -1;
    LmbGgufInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    LmbV4Inventory specs={.m=m,.h=c.h,.heads=c.heads,.hd=c.hd,.qr=c.qr,.og=c.og,.orr=c.orr,.inter=c.inter,
        .hc=c.hc,.ih=c.ih,.id=c.id,.window=c.window,.topk=c.topk,.hash=c.hash};
    m->experts=c.e;m->experts_per_tok=c.k;m->moe_intermediate=c.inter;
    for(uint32_t i=0;i<c.layers;i++)specs.ratios[i]=c.ratios[i];
    int rc=-1;uint64_t H=c.h,HC=c.hc,expert_cost[LMB_PLAN_LAYER_MAX]={0},largest_bank=0;char name[160];
#define VW(n,r,x,y,z,mat,cost) if(lmb_gguf_inventory_weight(&v,n,r,x,y,z,mat,cost))goto done
    VW("token_embd.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);VW("output.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    const char *globals[]={"output_norm.weight","output_hc_fn.weight","output_hc_base.weight","output_hc_scale.weight"};
    uint64_t x[]={H,HC*H,HC,1},y[]={1,HC,1,1};
    for(unsigned i=0;i<4;i++) {
        const LmbGgufTensor *t=lmb_gguf_tensor(g,globals[i]);
        if(!t || (t->type!=0 && t->type!=1 && t->type!=30))goto done;
        VW(globals[i],i==1?2:1,x[i],y[i],1,0,&m->edge_resident_bytes);
    }
    for(uint32_t l=0;l<c.layers;l++) {
        LmbV4Spec s[48];unsigned count=lmb_v4_specs(&specs,l,s);uint64_t *cost=&m->memory[l].resident_bytes;
        for(unsigned i=0;i<count;i++) {
            if(!strcmp(s[i].dtype,"F8_E8M0"))continue;
            const char *gg=lmb_v4g_name(s[i].name);if(!gg)goto done;
            snprintf(name,sizeof name,"blk.%u.%s",l,gg);const LmbGgufTensor *t=lmb_gguf_tensor(g,name);
            if(!t)goto done;
            if(!strcmp(s[i].dtype,"I64")) {
                if((t->type!=26 && t->type!=27) || t->rank!=2 || t->shape[0]!=s[i].cols || t->shape[1]!=s[i].rows)goto done;
                v.seen[t-g->tensors]=1;*cost=lmb_size_add(*cost,lmb_size_mul(s[i].cols*s[i].rows,8));continue;
            }
            if(!s[i].cols && !strcmp(s[i].dtype,"BF16") && t->type!=0 && t->type!=1 && t->type!=30)goto done;
            if((!strcmp(s[i].name,"ffn.gate.weight") || !strcmp(s[i].name,"attn.indexer.weights_proj.weight")) &&
               t->type!=0 && t->type!=1 && t->type!=30)goto done;
            VW(name,s[i].cols?2:1,s[i].cols?s[i].cols:s[i].rows,s[i].cols?s[i].rows:1,1,
                strcmp(s[i].dtype,"F32")!=0 && s[i].cols!=0,cost);
        }
        uint64_t slot;if(lmb_v4g_experts(&v,&c,l,&slot))goto done;
        expert_cost[l]=lmb_size_mul(slot,c.e);*cost=lmb_size_add(*cost,expert_cost[l]);
        if(*cost==UINT64_MAX)goto done;
        if(expert_cost[l]>largest_bank)largest_bank=expert_cost[l];
        uint64_t ratio=c.ratios[l];LmbLayerMemory *mem=&m->memory[l];
        mem->state_fixed_bytes=(uint64_t)c.window*c.hd*4+4096;
        if(ratio) {
            uint64_t overlap=ratio==4?2:1;
            mem->state_fixed_bytes=lmb_size_add(mem->state_fixed_bytes,ratio*overlap*overlap*c.hd*8+(uint64_t)16*c.hd*4);
            mem->state_token_bytes=(uint64_t)c.hd*16/ratio+16;
        }
        if(ratio==4) {
            uint64_t wide=(uint64_t)c.ih*c.id+c.ih+c.qr+c.id;
            mem->state_fixed_bytes=lmb_size_add(mem->state_fixed_bytes,(uint64_t)128*c.id*5+65536+wide*128*16);
            mem->state_token_bytes=lmb_size_add(mem->state_token_bytes,(uint64_t)c.id*4+256);
        }
        mem->state_fixed_bytes=lmb_size_mul(mem->state_fixed_bytes,2);
    }
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i])goto done;
    /* The upstream store allocates every slot at the largest record size
     * across the complete checkpoint, even for a partial layer interval. */
    for(uint32_t i=0;i<c.layers;i++)m->memory[i].resident_bytes=lmb_size_add(m->memory[i].resident_bytes,largest_bank-expert_cost[i]);
    uint64_t index=lmb_size_add(lmb_size_mul(g->header_bytes,6),lmb_size_mul(g->ntensors,1024));
    m->segment_fixed_bytes=lmb_size_add(index,(uint64_t)c.layers*(8192u+(uint64_t)c.e*2048u));
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,index);
    uint64_t wide=H*HC+(uint64_t)c.heads*c.hd*3+c.qr+(uint64_t)c.og*c.orr+
        (uint64_t)c.k*(H+c.inter)+c.e+(uint64_t)c.ih*c.id;
    m->scratch_fixed_bytes=lmb_size_add(UINT64_C(64)<<20,lmb_size_add(v.largest*2,lmb_size_mul(wide,128u*16u*4u)));
    m->scratch_token_bytes=(uint64_t)c.hd*4+1024;m->session_fixed_bytes=(uint64_t)c.layers*16;
    m->edge_scratch_fixed_bytes=(uint64_t)c.vocab*8+H*128*8;
    m->boundary_width=c.h*c.hc;m->max_context=c.context;m->memory_contract=1;rc=0;
done:
#undef VW
    free(v.seen);return rc;
}
#endif
