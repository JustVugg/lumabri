#ifndef LMB_PLAN_INKLING_GGUF_H
#define LMB_PLAN_INKLING_GGUF_H
#include "gguf_inventory.h"

typedef struct {
    uint32_t layers,h,vocab,unpad,heads,kv,local_kv,hd,window,rel,extent,conv;
    uint32_t experts,topk,shared,dense,inter,first,context,floor;
    uint8_t local[256];
    double eps,scale,mup,alpha;
} LmbInkGguf;
static const LmbGgufValue *lmb_ink_value(const LmbGguf *g,const char *key) {
    char name[128];int n=snprintf(name,sizeof name,"inkling.%s",key);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_ink_uint(const LmbGguf *g,const char *key,uint32_t def,uint32_t lo,uint32_t hi,uint32_t *out) {
    const LmbGgufValue *v=lmb_ink_value(g,key);uint64_t n=def;
    if((v && lmb_gguf_integer(v,&n)) || n<lo || n>hi)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_ink_geometry(const LmbGguf *g,LmbInkGguf *c) {
    memset(c,0,sizeof *c);const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"inkling"))return -1;
#define IU(k,f,d,lo,hi) if(lmb_ink_uint(g,k,d,lo,hi,&c->f))return -1
    IU("block_count",layers,0,1,256);IU("embedding_length",h,0,1,65536);
    IU("attention.head_count",heads,0,1,1024);IU("attention.key_length",hd,0,1,65536);
    IU("attention.sliding_window",window,0,1,1048576);IU("d_rel",rel,0,1,65536);
    IU("rel_extent",extent,0,1,1048576);IU("shortconv_kernel",conv,0,2,1024);
    IU("expert_count",experts,0,1,4096);IU("expert_used_count",topk,0,1,256);
    IU("expert_shared_count",shared,0,1,64);IU("feed_forward_length",dense,0,1,1048576);
    IU("expert_feed_forward_length",inter,0,1,1048576);IU("dense_block_count",first,0,0,c->layers);
    IU("context_length",context,1048576,1,1048576);IU("log_scaling_n_floor",floor,0,0,UINT32_MAX);
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || !tokens->count || tokens->count>1048576)return -1;
    c->vocab=(uint32_t)tokens->count;
    uint32_t vocab; if(lmb_ink_uint(g,"vocab_size",c->vocab,c->vocab,c->vocab,&vocab))return -1;
    IU("unpadded_vocab_size",unpad,c->vocab,1,c->vocab);
#undef IU
    const LmbGgufValue *pattern=lmb_ink_value(g,"attention.sliding_window_pattern"),*kv=lmb_ink_value(g,"attention.head_count_kv");
    if(!pattern || pattern->type!=9 || pattern->count!=c->layers || !kv ||
        (kv->type==9 && kv->count!=c->layers) || c->topk>c->experts || (uint64_t)c->heads*c->hd>1048576)return -1;
    for(uint32_t i=0;i<c->layers;i++) {
        uint64_t local,n;
        if(lmb_gguf_inventory_item(pattern,i,&local) || local>1 ||
            (kv->type==9?lmb_gguf_inventory_item(kv,i,&n):lmb_gguf_integer(kv,&n)) || !n || n>c->heads || c->heads%n)return -1;
        c->local[i]=(uint8_t)local;uint32_t *count=local?&c->local_kv:&c->kv;
        if(*count && *count!=n)return -1;
        *count=(uint32_t)n;
    }
    if(!c->kv)c->kv=c->local_kv;
    if(!c->local_kv)c->local_kv=c->kv;
    if(lmb_gguf_number(lmb_ink_value(g,"attention.layer_norm_rms_epsilon"),1e-6,&c->eps) ||
        lmb_gguf_number(lmb_ink_value(g,"expert_weights_scale"),1,&c->scale) ||
        lmb_gguf_number(lmb_ink_value(g,"logit_scale_denom"),NAN,&c->mup) ||
        lmb_gguf_number(lmb_ink_value(g,"log_scaling_alpha"),0,&c->alpha))return -1;
    return c->eps>0 && c->eps<=FLT_MAX && c->scale>0 && c->scale<=FLT_MAX &&
        c->mup>0 && c->mup<=FLT_MAX && c->alpha>=0 && c->alpha<=FLT_MAX?0:-1;
}
static int lmb_ink_matches(const char *cfg,const LmbInkGguf *c) {
    const char *keys[]={"num_hidden_layers","hidden_size","vocab_size","unpadded_vocab_size","num_attention_heads",
        "swa_num_attention_heads","num_key_value_heads","swa_num_key_value_heads","head_dim","swa_head_dim",
        "sliding_window_size","d_rel","rel_extent","conv_kernel_size","sconv_kernel_size","n_routed_experts",
        "num_experts_per_tok","n_shared_experts","intermediate_size","moe_intermediate_size","dense_mlp_idx",
        "max_position_embeddings","log_scaling_n_floor"};
    uint32_t values[]={c->layers,c->h,c->vocab,c->unpad,c->heads,c->heads,c->kv,c->local_kv,c->hd,c->hd,
        c->window,c->rel,c->extent,c->conv,c->conv,c->experts,c->topk,c->shared,c->dense,c->inter,c->first,c->context,c->floor};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++){uint32_t n;if(lmb_plan_u32(cfg,keys[i],values[i],values[i],&n))return -1;}
    uint8_t local[256]={0},sparse[256];uint64_t ids[256];unsigned count;
    if(lmb_plan_array(lmb_json_member(cfg,"local_layer_ids"),ids,256,&count))return -1;
    for(unsigned i=0;i<count;i++){if(ids[i]>=c->layers || local[ids[i]])return -1;local[ids[i]]=1;}
    if(memcmp(local,c->local,c->layers) ||
        lmb_plan_kinds(cfg,"mlp_layer_types","dense","sparse",sparse,c->layers))return -1;
    for(uint32_t i=0;i<c->layers;i++)if(sparse[i]!=(i>=c->first))return -1;
    if(lmb_json_member(cfg,"layer_types") &&
        (lmb_plan_kinds(cfg,"layer_types","hybrid","hybrid_sliding",local,c->layers) || memcmp(local,c->local,c->layers)))return -1;
    if(lmb_json_member(cfg,"dense_intermediate_size") || lmb_json_member(cfg,"audio_config"))return -1;
    const char *kf[]={"rms_norm_eps","route_scale","logits_mup_width_multiplier","log_scaling_alpha"};
    double vals[]={c->eps,c->scale,c->mup,c->alpha};
    for(unsigned i=0;i<4;i++){double x;if(lmb_plan_number(cfg,kf[i],NAN,&x) || (float)x!=(float)vals[i])return -1;}
    return 0;
}
static int lmb_ink_experts(LmbGgufInventory *v,const LmbInkGguf *c,uint32_t layer,uint64_t *gu,uint64_t *down) {
    char name[128];snprintf(name,sizeof name,"blk.%u.ffn_exps_colibri",layer);
    const LmbGgufTensor *packed=lmb_gguf_tensor(v->g,name);
    const char *layout=lmb_gguf_text(lmb_gguf_get(v->g,"colibri.layout"));
    if(layout && strcmp(layout,"expert-packed-v1"))return -1;
    const LmbGgufValue *pt=NULL,*pb=NULL,*ps=NULL,*pd=NULL;uint64_t stride=0,total=0,held[3];
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
        uint64_t cols=k==2?c->inter:c->h,rows=k==2?c->h:c->inter,type,bytes;
        if(layout) {
            uint64_t x,y;
            if(lmb_gguf_inventory_item(pt,3*layer+k,&type) || lmb_gguf_inventory_item(pb,3*layer+k,&bytes) ||
                lmb_gguf_inventory_item(pd,6*layer+2*k,&x) || lmb_gguf_inventory_item(pd,6*layer+2*k+1,&y) || x!=cols || y!=rows)return -1;
        } else {
            snprintf(name,sizeof name,"blk.%u.ffn_%s_exps.weight",layer,suffix[k]);const LmbGgufTensor *t=lmb_gguf_tensor(v->g,name);
            if(!t || t->rank!=3 || t->shape[0]!=cols || t->shape[1]!=rows || t->shape[2]!=c->experts)return -1;
            type=t->type;bytes=t->bytes/c->experts;v->seen[t-v->g->tensors]=1;
        }
        if(type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES)return -1;
        const LmbGgufType *q=&LMB_GGUF_TYPES[type];
        if(!q->decodable || cols%q->block || bytes!=cols/q->block*q->bytes*rows)return -1;
        LmbGgufTensor fake={.rank=2,.type=(uint32_t)type,.shape={cols,rows}};
        held[k]=lmb_gguf_held_bytes(&fake,0,1);if(held[k]==UINT64_MAX)return -1;
        total=lmb_size_add(total,bytes);
    }
    if(total==UINT64_MAX || (layout && total>stride))return -1;
    if(layout)v->seen[packed-v->g->tensors]=1;
    *gu=(held[0]/c->inter+held[1]/c->inter+1)/2;*down=held[2]/c->h;return 0;
}
static int lmb_ink_gguf_memory(const LmbGguf *g,const char *cfg,LmbModelShape *m) {
    LmbInkGguf c;if(lmb_ink_geometry(g,&c) || lmb_ink_matches(cfg,&c) || m->layers!=c.layers || m->hidden!=c.h || m->vocab!=c.vocab)return -1;
    LmbGgufInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    uint64_t H=c.h,gu_max=0,down_max=0;char name[128];int rc=-1;
#define IW(n,r,x,y,z,mat,cost) if(lmb_gguf_inventory_weight(&v,n,r,x,y,z,mat,cost))goto done
    IW("token_embd.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    IW("output.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    IW("output_norm.weight",1,H,1,1,0,&m->edge_resident_bytes);
    if(lmb_gguf_tensor(g,"token_embd_norm.weight"))IW("token_embd_norm.weight",1,H,1,1,0,&m->edge_resident_bytes);
    for(uint32_t l=0;l<c.layers;l++) {
        uint64_t *cost=&m->memory[l].resident_bytes,K=(uint64_t)(c.local[l]?c.local_kv:c.kv)*c.hd,Q=(uint64_t)c.heads*c.hd;
#define LW(s,r,x,y,z,mat) do{snprintf(name,sizeof name,"blk.%u.%s",l,s);IW(name,r,x,y,z,mat,cost);}while(0)
        LW("attn_norm.weight",1,H,1,1,0);LW("ffn_norm.weight",1,H,1,1,0);
        LW("attn_q.weight",2,H,Q,1,1);LW("attn_k.weight",2,H,K,1,1);LW("attn_v.weight",2,H,K,1,1);
        LW("attn_r.weight",2,H,(uint64_t)c.heads*c.rel,1,1);LW("attn_output.weight",2,Q,H,1,1);
        LW("attn_q_norm.weight",1,c.hd,1,1,0);LW("attn_k_norm.weight",1,c.hd,1,1,0);
        LW("attn_rel_proj.weight",2,c.local[l]?c.window:c.extent,c.rel,1,0);
        LW("shortconv_k.weight",2,c.conv,K,1,0);LW("shortconv_v.weight",2,c.conv,K,1,0);
        LW("shortconv_attn.weight",2,c.conv,H,1,0);LW("shortconv_mlp.weight",2,c.conv,H,1,0);
        if(l<c.first) {
            LW("ffn_gate.weight",2,H,c.dense,1,1);LW("ffn_up.weight",2,H,c.dense,1,1);
            LW("ffn_down.weight",2,c.dense,H,1,1);
        } else {
            const char *shared[]={"gate","up","down"};
            for(unsigned k=0;k<3;k++) {
                snprintf(name,sizeof name,"blk.%u.ffn_%s_shexp.weight",l,shared[k]);
                const LmbGgufTensor *t=lmb_gguf_tensor(g,name);
                uint64_t x=k==2?c.inter:H,y=k==2?H:c.inter;
                if(t && t->rank==3) {IW(name,3,x,y,c.shared,1,cost);}
                else {IW(name,2,x,y*c.shared,1,1,cost);}
            }
        }
        snprintf(name,sizeof name,"blk.%u.ffn_gscale.weight",l);
        if(lmb_gguf_tensor(g,name))IW(name,1,1,1,1,0,cost);
        if(l>=c.first) {
            LW("ffn_gate_inp.weight",2,H,c.experts+c.shared,1,0);LW("exp_probs_b.bias",1,c.experts,1,1,0);
            uint64_t gu,dn;if(lmb_ink_experts(&v,&c,l,&gu,&dn))goto done;
            if(gu>gu_max)gu_max=gu;
            if(dn>down_max)down_max=dn;
        }
        m->memory[l].state_token_bytes=K*8;m->memory[l].state_context_limit=c.local[l]?c.window:0;
        m->memory[l].state_fixed_bytes=2*(K+H)*(c.conv-1)*4;
#undef LW
    }
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i])goto done;
    uint64_t slot=2u*c.inter*gu_max+H*down_max+(2u*c.inter+H)*4u;
    for(uint32_t l=c.first;l<c.layers;l++)m->memory[l].resident_bytes=lmb_size_add(m->memory[l].resident_bytes,slot*c.experts+32768u);
    uint64_t index=lmb_size_add(g->header_bytes*6u,((uint64_t)g->ntensors+c.layers*c.experts*3u)*1024u);
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,index);
    m->edge_scratch_fixed_bytes=lmb_size_add(v.largest,(H*128+c.vocab)*4u);
    uint64_t wide=H+(uint64_t)(2*c.heads+2*c.kv+2*c.local_kv)*c.hd+2u*c.heads*c.rel+
        (uint64_t)c.shared*(H+c.inter)+(uint64_t)c.topk*(H+c.inter)+c.experts+c.dense;
    m->scratch_fixed_bytes=lmb_size_add(v.largest,wide*128u*16u*4u+H*c.inter*24u+(uint64_t)(c.extent+c.window+c.conv+128u)*256u*4u);
    m->scratch_token_bytes=256u*4u;m->segment_fixed_bytes=lmb_size_add(index,(uint64_t)c.layers*(4096u+c.experts*256u));
    m->experts=c.experts;m->experts_per_tok=c.topk;m->moe_intermediate=c.inter;m->max_context=c.context;m->memory_contract=1;rc=0;
done:free(v.seen);return rc;
#undef IW
}
#endif
