/* Native qwen36 GGUF geometry and loaded CPU representation. A GGUF file's
 * size is not a RAM budget: vectors/embedding expand and experts may repack. */
#ifndef LMB_PLAN_QWEN36_GGUF_H
#define LMB_PLAN_QWEN36_GGUF_H
#include "olmoe_gguf.h"

typedef struct {
    const char *arch,*model_type;
    uint32_t layers,hidden,vocab,heads,kv,hd,experts,topk,inter,shared,dense_inter,lead;
    uint32_t vh,kh,kdim,vdim,convk,convdim,context,window,interval,mtp;
    uint8_t full[LMB_PLAN_LAYER_MAX],dense[LMB_PLAN_LAYER_MAX];
    unsigned lfm,plain;
} LmbQwenGguf;

static const LmbGgufValue *lmb_qg_value(const LmbGguf *g,const LmbQwenGguf *c,const char *suffix) {
    char key[128];int n=snprintf(key,sizeof key,"%s.%s",c->arch,suffix);
    return n>0 && (size_t)n<sizeof key?lmb_gguf_get(g,key):NULL;
}
static int lmb_qg_u32(const LmbGguf *g,const LmbQwenGguf *c,const char *key,
                     uint32_t fallback,uint32_t minimum,uint32_t maximum,uint32_t *out) {
    const LmbGgufValue *v=lmb_qg_value(g,c,key);uint64_t n=fallback;
    if((v && lmb_gguf_integer(v,&n)) || n<minimum || n>maximum)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_qg_array(const LmbGgufValue *v,uint32_t index,uint64_t *out) {
    if(!v || v->type!=9 || index>=v->count)return -1;
    unsigned size=lmb_gguf_scalar_bytes(v->element_type);
    if(!size || (uint64_t)(index+1)*size>v->bytes)return -1;
    LmbGgufValue item={.type=v->element_type,.bytes=size,.data=v->data+(size_t)index*size};
    return lmb_gguf_integer(&item,out);
}
static int lmb_qg_geometry(const LmbGguf *g,LmbQwenGguf *c) {
    memset(c,0,sizeof *c);
    c->arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!c->arch)return -1;
    static const char *const arches[]={"qwen3","qwen3moe","qwen35","qwen35moe","flint","lfm3"};
    static const char *const types[]={"qwen3","qwen3_moe","qwen3_5","qwen3_5_moe","flint","lfm3"};
    unsigned kind=0;while(kind<6 && strcmp(c->arch,arches[kind]))kind++;
    if(kind==6)return -1;
    c->model_type=types[kind];c->lfm=kind==5;c->plain=kind==0 || kind==1 || kind==4;
#define QG(key,field,def,lo,hi) if(lmb_qg_u32(g,c,key,def,lo,hi,&c->field))return -1
    QG("block_count",layers,0,1,LMB_PLAN_LAYER_MAX);
    QG("nextn_predict_layers",mtp,0,0,c->layers-1);c->layers-=c->mtp;
    QG("embedding_length",hidden,0,1,65536);
    QG("attention.head_count",heads,0,1,65536);
    QG("attention.key_length",hd,c->hidden/c->heads,1,65536);
    uint32_t vd;
    if(lmb_qg_u32(g,c,"attention.value_length",c->hd,1,65536,&vd) || vd!=c->hd)return -1;
    const LmbGgufValue *kv=lmb_qg_value(g,c,"attention.head_count_kv");
    if(kv && kv->type==9) {
        if(kv->count!=c->layers || (kv->element_type!=4 && kv->element_type!=5))return -1;
        for(uint32_t i=0;i<c->layers;i++) {
            uint64_t v;if(lmb_qg_array(kv,i,&v) || v>c->heads)return -1;
            if(v>c->kv)c->kv=(uint32_t)v;
        }
    } else {QG("attention.head_count_kv",kv,0,1,c->heads);}
    if(!c->kv || c->heads%c->kv)return -1;
    QG("expert_count",experts,0,0,1024);QG("expert_used_count",topk,0,0,256);
    QG("expert_feed_forward_length",inter,0,0,65536);
    QG("expert_shared_feed_forward_length",shared,0,0,1048576);
    QG("feed_forward_length",dense_inter,0,0,1048576);
    if(kind==0 || kind==2 || c->lfm) {
        if(c->experts || c->topk || c->inter || !c->dense_inter)return -1;
        c->shared=c->dense_inter;
    } else if(!c->experts || !c->topk || c->topk>c->experts || !c->inter)return -1;
    QG("context_length",context,262144,1,UINT32_MAX);
    if(c->context>262144)c->context=262144;
    QG("attention.sliding_window",window,0,0,1048576);
    QG("full_attention_interval",interval,4,1,LMB_PLAN_LAYER_MAX);
    if(c->plain)c->interval=1;
    if(kind==4) {QG("leading_dense_block_count",lead,0,0,c->layers);}
    if(c->lead && !c->dense_inter)return -1;
    if(c->lfm) {
        QG("shortconv.l_cache",convk,0,2,64);c->convdim=c->hidden;
    } else if(!c->plain) {
        QG("ssm.group_count",kh,0,1,65536);QG("ssm.time_step_rank",vh,0,1,65536);
        QG("ssm.state_size",kdim,0,1,512);QG("ssm.conv_kernel",convk,0,2,65536);
        uint32_t inner;if(lmb_qg_u32(g,c,"ssm.inner_size",0,1,1048576,&inner) ||
            inner%c->vh || c->vh%c->kh)return -1;
        c->vdim=inner/c->vh;
        uint64_t dim=2u*(uint64_t)c->kh*c->kdim+inner;
        if(!c->vdim || c->vdim>512 || dim>1048576)return -1;
        c->convdim=(uint32_t)dim;
    }
#undef QG
    const LmbGgufTensor *embed=lmb_gguf_tensor(g,"token_embd.weight");
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!embed || embed->rank!=2 || embed->shape[0]!=c->hidden || !tokens || tokens->type!=9 ||
       tokens->element_type!=8 || !tokens->count || tokens->count>1048576 || embed->shape[1]!=tokens->count)return -1;
    c->vocab=(uint32_t)tokens->count;
    const LmbGgufValue *windows=lmb_qg_value(g,c,"attention.sliding_window_pattern");
    const LmbGgufValue *ropes=lmb_qg_value(g,c,"attention.rope_pattern");
    if(kind==4 || c->lfm) {
        if(!c->window || !windows || windows->type!=9 || windows->element_type!=7 || windows->count!=c->layers)return -1;
        if(kind==4 && (!ropes || ropes->type!=9 || ropes->element_type!=7 || ropes->count!=c->layers))return -1;
    }
    for(uint32_t i=0;i<c->layers;i++) {
        char name[96];snprintf(name,sizeof name,"blk.%u.attn_q.weight",i);
        c->full[i]=lmb_gguf_tensor(g,name)!=NULL;c->dense[i]=!c->experts || i<c->lead;
        if(!c->lfm && c->full[i]!=((i+1)%c->interval==0))return -1;
        if(c->lfm && kv && kv->type==9) {
            uint64_t v;if(lmb_qg_array(kv,i,&v) || (v!=0)!=c->full[i] || (v && v!=c->kv))return -1;
        }
        if(windows) {
            uint64_t v;if(lmb_qg_array(windows,i,&v) || v>1 || (v && !c->full[i]))return -1;
        }
        if(ropes) {uint64_t v;if(lmb_qg_array(ropes,i,&v) || v>1)return -1;}
    }
    return 0;
}

static int lmb_qwen_gguf_memory(const LmbGguf *g,LmbModelShape *m) {
    LmbQwenGguf c;if(lmb_qg_geometry(g,&c))return -1;
    const char *layout=lmb_gguf_text(lmb_gguf_get(g,"colibri.layout"));
    int packed=layout && !strcmp(layout,"expert-packed-v1");
    if(layout && !packed)return -1;
    const LmbGgufValue *pt=lmb_gguf_get(g,"colibri.expert_types"),
        *pb=lmb_gguf_get(g,"colibri.expert_bytes"),*ps=lmb_gguf_get(g,"colibri.expert_stride");
    if(packed && (!pt || !pb || !ps || pt->type!=9 || pt->element_type!=4 || pt->count!=3u*c.layers ||
        pb->type!=9 || pb->element_type!=10 || pb->count!=3u*c.layers ||
        ps->type!=9 || ps->element_type!=10 || ps->count!=c.layers))return -1;
    if(strcmp(m->model_type,c.model_type) || m->hidden!=c.hidden || m->layers!=c.layers ||
       m->vocab!=c.vocab || m->experts!=c.experts || m->experts_per_tok!=c.topk)return -1;
    /* The field order matches the converted contract, but sizes describe
     * native matrices: no unpacked int8 or qwen36_meta.json is assumed. */
    const char *names[Q36_FIELDS]={"attn_norm.weight",c.plain||c.lfm?"ffn_norm.weight":"post_attention_norm.weight",
        "ffn_gate_inp.weight","ffn_gate_shexp.weight","ffn_up_shexp.weight","ffn_down_shexp.weight","ffn_gate_inp_shexp.weight",
        "attn_q_norm.weight","attn_k_norm.weight","exp_probs_b.bias","attn_q.weight","attn_k.weight","attn_v.weight","attn_output.weight",
        "attn_qkv.weight","attn_gate.weight","ssm_beta.weight","ssm_alpha.weight","ssm_conv1d.weight","ssm_dt.bias","ssm_a",
        "ssm_norm.weight","ssm_out.weight","ffn_gate.weight","ffn_up.weight","ffn_down.weight","attn_sinks.weight",
        "post_attention_norm.weight","post_ffw_norm.weight","attn_gate.weight","shortconv.in_proj.weight","shortconv.conv.weight","shortconv.out_proj.weight"};
    uint64_t H=c.hidden,DV=(uint64_t)c.vh*c.vdim,qd=(uint64_t)c.hd*(c.plain||c.lfm?1:2);
    uint64_t counts[Q36_FIELDS]={H,H,H*c.experts,H*c.shared,H*c.shared,H*c.shared,H,c.hd,c.hd,c.experts,
        H*c.heads*qd,H*c.kv*c.hd,H*c.kv*c.hd,H*c.heads*c.hd,H*c.convdim,H*DV,H*c.vh,H*c.vh,
        (uint64_t)c.convdim*c.convk,c.vh,c.vh,c.vdim,H*DV,0,0,0,c.heads,H,H,H*c.heads,3*H*H,H*c.convk,H*H};
    uint64_t seen[LMB_PLAN_LAYER_MAX]={0},expert[LMB_PLAN_LAYER_MAX][3]={{0}},largest=0,maxslot=0;
    unsigned edges=0;
    for(uint32_t i=0;i<g->ntensors;i++) {
        const LmbGgufTensor *t=&g->tensors[i];uint64_t count=1;
        for(uint32_t d=0;d<t->rank;d++)count=lmb_size_mul(count,t->shape[d]);
        if(count==UINT64_MAX)return -1;
        size_t len=strlen(t->name),suffix=len>6 && !strcmp(t->name+len-6,".scale")?6:
            len>12 && !strcmp(t->name+len-12,".input_scale")?12:0;
        if(suffix) {
            char weight[256];
            int n=snprintf(weight,sizeof weight,"%.*s.weight",(int)(len-suffix),t->name);
            if(n<0 || (size_t)n>=sizeof weight || t->type!=0)return -1;
            if(!strcmp(weight,"rope_freqs.weight"))return -1;
            const LmbGgufTensor *w=lmb_gguf_tensor(g,weight);
            uint64_t expected=w && w->rank==3?w->shape[2]:1;
            if(!w) {
                uint64_t layer;const char *field=!strncmp(weight,"blk.",4)?lmb_plan_uint(weight+4,&layer):NULL;
                if(!field || *field++!='.' || layer>=(uint64_t)c.layers+c.mtp)return -1;
                if(layer>=c.layers)continue;
                if(!packed || c.dense[layer] || (strcmp(field,"ffn_gate_exps.weight") &&
                    strcmp(field,"ffn_up_exps.weight") && strcmp(field,"ffn_down_exps.weight")))return -1;
                expected=c.experts;
            }
            if(count!=expected && !(suffix==12 && count==1))return -1;
            if(suffix==6) {
                m->segment_fixed_bytes=lmb_size_add(m->segment_fixed_bytes,count*4);
                m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,count*4);
            }
            continue;
        }
        int e=!strcmp(t->name,"token_embd.weight")?0:!strcmp(t->name,"output.weight")?1:!strcmp(t->name,"output_norm.weight")?2:-1;
        if(e>=0) {
            if((edges&(1u<<e)) || t->rank!=(e==2?1u:2u) || t->shape[0]!=H || (e!=2 && t->shape[1]!=c.vocab))return -1;
            edges|=1u<<e;
            uint64_t cost=e==1?lmb_gguf_held_bytes(t,1,1):lmb_size_mul(count,4);
            if(!LMB_GGUF_TYPES[t->type].decodable || cost==UINT64_MAX)return -1;
            m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,cost);
            if(cost>m->edge_scratch_fixed_bytes)m->edge_scratch_fixed_bytes=cost;
            continue;
        }
        if(c.lfm && !strcmp(t->name,"rope_freqs.weight")) {
            if(t->rank!=1 || count!=c.hd/2 || !LMB_GGUF_TYPES[t->type].decodable)return -1;
            continue;
        }
        if(strncmp(t->name,"blk.",4))return -1;
        uint64_t l;const char *name=lmb_plan_uint(t->name+4,&l);
        if(!name || *name++!='.' || l>=(uint64_t)c.layers+c.mtp)return -1;
        if(l>=c.layers)continue; /* explicitly declared MTP blocks */
        if(!strcmp(name,"ffn_exps_colibri")) {
            uint64_t stride,total=0;
            if(!packed || c.dense[l] || expert[l][0] || t->type!=24 || t->rank!=2 ||
                lmb_qg_array(ps,(uint32_t)l,&stride) || !stride || t->shape[0]!=stride || t->shape[1]!=c.experts)return -1;
            for(unsigned k=0;k<3;k++) {
                uint64_t type,bytes;
                if(lmb_qg_array(pt,(uint32_t)l*3+k,&type) || lmb_qg_array(pb,(uint32_t)l*3+k,&bytes) ||
                    type>=sizeof LMB_GGUF_TYPES/sizeof *LMB_GGUF_TYPES || !LMB_GGUF_TYPES[type].decodable)return -1;
                const LmbGgufType *format=&LMB_GGUF_TYPES[type];
                uint64_t cols=k==2?c.inter:H,rows=k==2?H:c.inter;
                if(cols%format->block || bytes!=lmb_size_mul(cols/format->block,format->bytes*rows))return -1;
                LmbGgufTensor weight={.type=(uint32_t)type,.rank=2,.shape={cols,rows}};
                expert[l][k]=lmb_gguf_held_bytes(&weight,0,1);
                if(expert[l][k]==UINT64_MAX)return -1;
                if(expert[l][k]>largest)largest=expert[l][k];
                total=lmb_size_add(total,bytes);
            }
            if(total>stride)return -1;
            continue;
        }
        unsigned k=0;const char *exps[]={"ffn_gate_exps.weight","ffn_up_exps.weight","ffn_down_exps.weight"};
        for(;k<3 && strcmp(name,exps[k]);k++);
        if(k<3) {
            if(packed || c.dense[l] || t->rank!=3 || t->shape[0]!=(k==2?c.inter:H) ||
                t->shape[1]!=(k==2?H:c.inter) || t->shape[2]!=c.experts || expert[l][k])return -1;
            uint64_t cost=lmb_gguf_held_bytes(t,0,1);if(cost==UINT64_MAX)return -1;
            expert[l][k]=cost;if(cost>largest)largest=cost;continue;
        }
        unsigned f=0;
        for(;f<Q36_FIELDS;f++) {
            if(f==Q36_DZ && c.lfm)continue;
            if(f==Q36_POST_OP && !c.lfm)continue;
            if(!strcmp(name,names[f]) || (f==Q36_DT && !strcmp(name,"ssm_dt")))break;
        }
        if(f==Q36_FIELDS || (seen[l]&(1ull<<f)))return -1;
        uint64_t need=f>=Q36_G && f<=Q36_D?H*(l<c.lead?c.dense_inter:c.shared):counts[f];
        if(!need || count!=need || !LMB_GGUF_TYPES[t->type].decodable)return -1;
        seen[l]|=1ull<<f;
        int matrix=(f>=Q36_Q && f<=Q36_O) || f==Q36_ROUTER || (f>=Q36_SG && f<=Q36_SD) ||
            f==Q36_DQKV || f==Q36_DZ || f==Q36_DOUT || (f>=Q36_G && f<=Q36_D) ||
            f==Q36_ATTN_GATE || f==Q36_SC_IN || f==Q36_SC_OUT;
        uint64_t columns=f==Q36_O?(uint64_t)c.heads*c.hd:f==Q36_SD?c.shared:
            f==Q36_DOUT?DV:f==Q36_D?(l<c.lead?c.dense_inter:c.shared):H;
        if(matrix && (t->rank!=2 || t->shape[0]!=columns || t->shape[1]!=need/columns))return -1;
        uint64_t cost=matrix?lmb_gguf_held_bytes(t,1,1):lmb_size_mul(count,4);
        if(cost==UINT64_MAX)return -1;
        m->memory[l].resident_bytes=lmb_size_add(m->memory[l].resident_bytes,cost);
        if(cost>largest)largest=cost;
    }
    if(!(edges&1) || !(edges&4))return -1;
    if(!(edges&2)) {
        const LmbGgufTensor *embed=lmb_gguf_tensor(g,"token_embd.weight");
        m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,lmb_gguf_held_bytes(embed,1,1));
    }
    int qknorm=(seen[c.layers-1]&(1ull<<Q36_QN))!=0;
    if(c.lfm)for(uint32_t l=0;l<c.layers;l++)if(c.full[l] && (seen[l]&(1ull<<Q36_QN)))qknorm=1;
    int sinks=!strcmp(c.arch,"flint") && (seen[0]&(1ull<<Q36_SINKS));
    for(uint32_t l=0;l<c.layers;l++) {
        uint64_t required=(1ull<<Q36_IN)|(1ull<<Q36_POST);
        if(!c.dense[l]) {
            required|=1ull<<Q36_ROUTER;
            if(!expert[l][0] || !expert[l][1] || !expert[l][2])return -1;
            uint64_t slot=lmb_size_add(expert[l][0],lmb_size_add(expert[l][1],expert[l][2]));
            if(slot>maxslot)maxslot=slot;
        }
        if(c.dense[l] || c.shared)required|=7ull<<(c.dense[l]?Q36_G:Q36_SG);
        required|=c.full[l]?15ull<<Q36_Q:c.lfm?7ull<<Q36_SC_IN:511ull<<Q36_DQKV;
        if(c.lfm)required|=(1ull<<Q36_POST_OP)|(1ull<<Q36_POST_FFN)|(c.full[l]?1ull<<Q36_ATTN_GATE:0);
        if(qknorm && c.full[l])required|=3ull<<Q36_QN;
        if(sinks)required|=1ull<<Q36_SINKS;
        uint64_t optional=(!c.dense[l]?1ull<<Q36_ROUTER_BIAS:0) |
            (c.shared && !c.dense[l]?1ull<<Q36_SHARED_GATE:0);
        if((seen[l]&required)!=required || (seen[l]&~(required|optional)))return -1;
        if(c.full[l] && ((seen[l]>>Q36_QN)&3u)!=0 && ((seen[l]>>Q36_QN)&3u)!=3u)return -1;
        m->memory[l].state_token_bytes=c.full[l]?(uint64_t)c.kv*c.hd*8:0;
        m->memory[l].state_fixed_bytes=c.full[l]?0:
            lmb_size_mul(lmb_size_add(c.lfm?1:(uint64_t)c.vh*c.kdim*c.vdim,(uint64_t)c.convdim*(c.convk-1)),4);
    }
    if(maxslot==UINT64_MAX)return -1;
    for(uint32_t l=0;l<c.layers;l++)if(!c.dense[l])
        m->memory[l].resident_bytes=lmb_size_add(m->memory[l].resident_bytes,lmb_size_mul(maxslot,c.experts));
    uint64_t headers=lmb_size_add(lmb_size_mul(g->header_bytes,6),lmb_size_mul(g->ntensors,512));
    m->segment_fixed_bytes=lmb_size_add(m->segment_fixed_bytes,lmb_size_add(headers,(uint64_t)c.layers*(8192u+(uint64_t)c.experts*512)));
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,headers);
    uint64_t widest=H+(uint64_t)c.heads*qd+2u*c.kv*c.hd+c.convdim+DV+c.inter+c.shared+c.dense_inter+c.experts;
    m->scratch_fixed_bytes=lmb_size_add(lmb_size_add(largest,maxslot),lmb_size_add(widest*128u*16u*4u,32u<<20));
    m->scratch_token_bytes=256u*4u;m->heads=c.heads;m->kv_heads=c.kv;m->moe_intermediate=c.inter;
    m->max_context=c.context;m->memory_contract=1;
    return m->scratch_fixed_bytes==UINT64_MAX || m->edge_resident_bytes==UINT64_MAX || m->segment_fixed_bytes==UINT64_MAX?-1:0;
}
#endif
