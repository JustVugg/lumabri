#ifndef LMB_V41_GGUF_H
#define LMB_V41_GGUF_H
#include "deepseek_v41_gguf_config.h"
/* Compare a canonical integer/string/array value independent of spacing.
 * Object members are selected separately, so sidecar key order is irrelevant. */
static int lmb_v41g_equal_value(const char *a,const char *b) {
    if(!a || !b)return 0;
    unsigned depth=0;int string=0;
    for(;;) {
        if(!string){a=lmb_plan_space(a);b=lmb_plan_space(b);}
        if(!string && !depth && (!*a || *a==',' || *a=='}') && (!*b || *b==',' || *b=='}'))return 1;
        if(*a!=*b)return 0;
        if(!*a)return 0;
        if(*a=='"')string=!string;
        else if(!string && *a=='[')depth++;
        else if(!string && *a==']'){if(!depth)return 0;depth--;}
        a++;b++;
    }
}
static int lmb_v41g_sidecar_matches(const char *root,const LmbGgufJson *expected) {
    char *actual=lmb_v41_read_json(root,"dsv41_engram.json",32u<<20,NULL);
    if(!expected->length){if(actual){free(actual);return -1;}return errno==ENOENT?0:-1;}
    if(!actual)return -1;
    const char *fields[]={"max_ngram_size","n_heads","head_dim","pad_id","layer_ids","num_embeddings",
        "primes","offsets","multipliers","token_map"};int rc=0;
    for(unsigned i=0;i<sizeof fields/sizeof *fields;i++)
        if(!lmb_v41g_equal_value(lmb_json_member(actual,fields[i]),lmb_json_member(expected->data,fields[i])))rc=-1;
    free(actual);return rc;
}
static const char *lmb_v41g_name(const char *hf) {
    static const struct {const char *hf,*gg;} names[]={
        {"attn.wq_a.weight","attn_q_a.weight"},{"attn.wq_b.weight","attn_q_b.weight"},
        {"attn.wkv.weight","attn_kv.weight"},{"attn.wo_a.weight","attn_output_a.weight"},
        {"attn.wo_b.weight","attn_output_b.weight"},{"attn.q_norm.weight","attn_q_a_norm.weight"},
        {"attn.kv_norm.weight","attn_kv_a_norm.weight"},{"attn.attn_sink","attn_sinks.weight"},
        {"attn_norm.weight","attn_norm.weight"},{"ffn_norm.weight","ffn_norm.weight"},
        {"hc_attn_fn","hc_attn_fn.weight"},{"hc_ffn_fn","hc_ffn_fn.weight"},
        {"hc_attn_base","hc_attn_base.weight"},{"hc_ffn_base","hc_ffn_base.weight"},
        {"hc_attn_scale","hc_attn_scale.weight"},{"hc_ffn_scale","hc_ffn_scale.weight"},
        {"ffn.gate.weight","ffn_gate_inp.weight"},{"ffn.gate.bias","exp_probs_b.bias"},
        {"ffn.shared_experts.w1.weight","ffn_gate_shexp.weight"},
        {"ffn.shared_experts.w3.weight","ffn_up_shexp.weight"},
        {"ffn.shared_experts.w2.weight","ffn_down_shexp.weight"},
        {"attn.compressor.wkv.weight","attn_compressor_kv.weight"},
        {"attn.compressor.wgate.weight","attn_compressor_gate.weight"},
        {"attn.compressor.norm.weight","attn_compressor_norm.weight"},
        {"attn.indexer.wk.weight","indexer.attn_k.weight"},
        {"attn.indexer.k_norm.weight","indexer.k_norm.weight"},
        {"attn.indexer.wq_b.weight","indexer.attn_q_b.weight"},
        {"attn.indexer.weights_proj.weight","indexer.proj.weight"},
        {"engram.wkv.weight","engram_kv.weight"},{"engram.q_weight","engram_q_norm.weight"},
        {"engram.k_weight","engram_k_norm.weight"},{"engram.embed.weight","engram_embd.weight"}};
    for(unsigned i=0;i<sizeof names/sizeof *names;i++)if(!strcmp(names[i].hf,hf))return names[i].gg;
    return NULL;
}
static int lmb_v41g_experts(LmbGgufInventory *v,const LmbV41Inventory *c,unsigned layer,uint64_t part[3]) {
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
        if(!q->decodable || cols%q->block || bytes!=lmb_size_mul(cols/q->block,lmb_size_mul(q->bytes,rows)))return -1;
        part[k]=bytes;total=lmb_size_add(total,bytes);
    }
    if(total==UINT64_MAX || (layout && total>stride))return -1;
    if(layout)v->seen[packed-v->g->tensors]=1;
    if(total>v->largest)v->largest=total;
    return 0;
}
static int lmb_v41g_tensors(const LmbGguf *g,LmbV41Inventory *c) {
    LmbGgufInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    LmbModelShape *m=c->m;int rc=-1;char name[160];uint64_t maximum[3]={0};
#define GW(n,r,x,y,z,mat,cost) if(lmb_gguf_inventory_weight(&v,n,r,x,y,z,mat,cost))goto done
    GW("token_embd.weight",2,c->h,c->vocab,1,1,&m->edge_resident_bytes);
    GW("output.weight",2,c->h,c->vocab,1,1,&m->edge_resident_bytes);
    const LmbGgufTensor *norm=lmb_gguf_tensor(g,"output_norm.weight");
    if(!norm || (norm->type!=0 && norm->type!=1 && norm->type!=30))goto done;
    GW("output_norm.weight",1,c->h,1,1,0,&m->edge_resident_bytes);
    for(unsigned l=0;l<c->layers;l++) {
        LmbV41Spec specs[64];unsigned n=lmb_v41_specs(c,l,specs);uint64_t *cost=&m->memory[l].resident_bytes;
        for(unsigned i=0;i<n;i++) {
            const LmbV41Spec *s=&specs[i];
            if(!strcmp(s->dtype,"F8_E8M0") || !strncmp(s->name,"engram.embed.",13))continue;
            const char *gg=lmb_v41g_name(s->name);if(!gg)goto done;
            snprintf(name,sizeof name,"blk.%u.%s",l,gg);const LmbGgufTensor *t=lmb_gguf_tensor(g,name);
            if(!t && (!strcmp(s->name,"engram.q_weight") || !strcmp(s->name,"engram.k_weight"))) {
                snprintf(name,sizeof name,"blk.%u.engram_gate.weight",l);t=lmb_gguf_tensor(g,name);
                if(!strcmp(s->name,"engram.k_weight") && t){*cost=lmb_size_add(*cost,lmb_size_mul(s->rows,s->cols*4));continue;}
            }
            if(!t || (s->widen && t->type!=0 && t->type!=1 && t->type!=30))goto done;
            GW(name,s->cols?2:1,s->cols?s->cols:s->rows,s->cols?s->rows:1,1,!s->widen,cost);
        }
        for(unsigned e=0;e<c->en;e++)if(c->el[e]==l) {
            snprintf(name,sizeof name,"blk.%u.engram_embd.weight",l);const LmbGgufTensor *t=lmb_gguf_tensor(g,name);
            uint64_t bytes;
            if(t) {
                if(t->type!=24 || t->rank!=2 || t->shape[0]!=c->ed+c->ed/32 || t->shape[1]!=c->erows[e] || c->ebegin[e])goto done;
                bytes=t->bytes;
            } else {
                t=lmb_gguf_tensor(g,"per_layer_token_embd.weight");
                if(!t || t->rank!=2 || t->shape[0]!=c->ed || t->shape[1]<c->erows[e] ||
                   !LMB_GGUF_TYPES[t->type].decodable || t->type==0 || t->type==1 || t->type==30)goto done;
                /* Only this layer's contiguous heads are prepared, not other
                 * layers' tables that happen to share the same GGUF tensor. */
                bytes=lmb_size_mul(c->erows[e]-c->ebegin[e],t->bytes/t->shape[1]);
            }
            v.seen[t-g->tensors]=1;*cost=lmb_size_add(*cost,bytes);if(bytes>v.largest)v.largest=bytes;
        }
        uint64_t part[3];if(lmb_v41g_experts(&v,c,l,part))goto done;
        for(unsigned k=0;k<3;k++)if(part[k]>maximum[k])maximum[k]=part[k];
        /* V4.1 ignores the optional vision router bias during text execution. */
        snprintf(name,sizeof name,"blk.%u.exp_probs_b_vl.bias",l);const LmbGgufTensor *vl=lmb_gguf_tensor(g,name);
        if(vl) {
            if(vl->rank!=1 || vl->shape[0]!=c->experts || (vl->type!=0 && vl->type!=1 && vl->type!=30))goto done;
            v.seen[vl-g->tensors]=1;
        }
    }
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i])goto done;
    uint64_t slot=lmb_size_add(lmb_size_add(maximum[0],maximum[1]),lmb_size_add(maximum[2],6u*8192u));
    for(unsigned l=0;l<c->layers;l++)m->memory[l].resident_bytes=lmb_size_add(m->memory[l].resident_bytes,lmb_size_mul(slot,c->experts));
    c->metadata=lmb_size_add(c->metadata,lmb_size_add(g->header_bytes*6,(uint64_t)g->ntensors*4096));
    c->largest=v.largest;rc=0;
done:
#undef GW
    free(v.seen);return rc;
}
#endif
