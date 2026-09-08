#ifndef LUMABRI_PLAN_DEEPSEEK_V4_H
#define LUMABRI_PLAN_DEEPSEEK_V4_H
/* Native V4: FP8 dense matrices with expanded scale banks, packed MXFP4
 * experts, mHC boundaries, window/compressor/indexer session state. */
typedef struct { char name[80]; const char *dtype; uint64_t rows,cols; } LmbV4Spec;
typedef struct { uint64_t offset[6]; uint32_t file; uint8_t seen; } LmbV4Expert;
typedef struct {
    LmbModelShape *m;
    uint32_t h,heads,hd,qr,og,orr,inter,hc,ih,id,window,topk,hash;
    uint64_t ratios[LMB_PLAN_LAYER_MAX],seen[LMB_PLAN_LAYER_MAX],largest;
    uint8_t edge;
    LmbV4Expert *expert;
} LmbV4Inventory;

static unsigned lmb_v4_specs(const LmbV4Inventory *v,uint32_t layer,LmbV4Spec *s) {
    unsigned n=0;
    uint64_t H=v->h,D=v->hd,Q=v->qr,HC=v->hc,P=(2+HC)*HC;
#define V4_SPEC(name_,dt,r,c) do { \
    snprintf(s[n].name,sizeof s[n].name,"%s",name_); s[n].dtype=dt; \
    s[n].rows=(r); s[n].cols=(c); n++; } while(0)
#define V4_FP8(prefix,r,c) do { V4_SPEC(prefix ".weight","F8_E4M3",r,c); \
    V4_SPEC(prefix ".scale","F8_E8M0",((r)+127)/128,((c)+127)/128); } while(0)
    V4_SPEC("attn.attn_sink","F32",v->heads,0);
    V4_SPEC("attn.kv_norm.weight","BF16",D,0);
    V4_SPEC("attn.q_norm.weight","BF16",Q,0);
    V4_FP8("attn.wkv",D,H);
    V4_FP8("attn.wo_a",(uint64_t)v->og*v->orr,(uint64_t)(v->heads/v->og)*D);
    V4_FP8("attn.wo_b",H,(uint64_t)v->og*v->orr);
    V4_FP8("attn.wq_a",Q,H); V4_FP8("attn.wq_b",(uint64_t)v->heads*D,Q);
    V4_SPEC("attn_norm.weight","BF16",H,0);
    uint64_t ratio=v->ratios[layer];
    if(ratio) {
        uint64_t coff=ratio==4 ? 2 : 1;
        V4_SPEC("attn.compressor.ape","F32",ratio,coff*D);
        V4_SPEC("attn.compressor.norm.weight","BF16",D,0);
        V4_SPEC("attn.compressor.wgate.weight","BF16",coff*D,H);
        V4_SPEC("attn.compressor.wkv.weight","BF16",coff*D,H);
    }
    if(ratio==4) {
        V4_SPEC("attn.indexer.compressor.ape","F32",4,2u*v->id);
        V4_SPEC("attn.indexer.compressor.norm.weight","BF16",v->id,0);
        V4_SPEC("attn.indexer.compressor.wgate.weight","BF16",2u*v->id,H);
        V4_SPEC("attn.indexer.compressor.wkv.weight","BF16",2u*v->id,H);
        V4_SPEC("attn.indexer.weights_proj.weight","BF16",v->ih,H);
        V4_FP8("attn.indexer.wq_b",(uint64_t)v->ih*v->id,Q);
    }
    V4_SPEC("ffn.gate.weight","BF16",v->m->experts,H);
    if(layer<v->hash) { V4_SPEC("ffn.gate.tid2eid","I64",v->m->vocab,v->m->experts_per_tok); }
    else { V4_SPEC("ffn.gate.bias","F32",v->m->experts,0); }
    V4_FP8("ffn.shared_experts.w1",v->inter,H);
    V4_FP8("ffn.shared_experts.w2",H,v->inter);
    V4_FP8("ffn.shared_experts.w3",v->inter,H);
    V4_SPEC("ffn_norm.weight","BF16",H,0);
    V4_SPEC("hc_attn_base","F32",P,0); V4_SPEC("hc_attn_fn","F32",P,HC*H);
    V4_SPEC("hc_attn_scale","F32",3,0);
    V4_SPEC("hc_ffn_base","F32",P,0); V4_SPEC("hc_ffn_fn","F32",P,HC*H);
    V4_SPEC("hc_ffn_scale","F32",3,0);
#undef V4_FP8
#undef V4_SPEC
    return n;
}
static int lmb_v4_shape(const LmbPlanTensor *t,const LmbV4Spec *s) {
    return !strcmp(t->dtype,s->dtype) && t->rank==(s->cols ? 2u : 1u) &&
        t->shape[0]==s->rows && (!s->cols || t->shape[1]==s->cols);
}
static int lmb_v4_tensor(const LmbPlanTensor *t,void *opaque) {
    LmbV4Inventory *v=opaque; LmbModelShape *m=v->m;
    const LmbV4Spec globals[]={
        {"embed.weight","BF16",m->vocab,v->h},{"head.weight","BF16",m->vocab,v->h},
        {"norm.weight","BF16",v->h,0},{"hc_head_fn","F32",v->hc,(uint64_t)v->hc*v->h},
        {"hc_head_base","F32",v->hc,0},{"hc_head_scale","F32",1,0}};
    for(unsigned i=0;i<6;i++) if(!strcmp(t->name,globals[i].name)) {
        if((v->edge&(1u<<i)) || !lmb_v4_shape(t,&globals[i])) return -1;
        v->edge|=1u<<i;
        /* Embedding and head are row-read by Edge. Full source-cache allowance
         * is deliberate, not a claim that the runtime pins the entire file. */
        uint64_t cost=i<2 ? t->bytes : lmb_size_mul(t->elements,4);
        m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,cost);
        return m->edge_resident_bytes==UINT64_MAX ? -1 : 0;
    }
    if(strncmp(t->name,"layers.",7)) return 0;
    uint64_t layer; const char *field=lmb_plan_uint(t->name+7,&layer);
    if(!field || *field++!='.') return -1;
    if(layer>=m->layers) return 0; /* MTP is not part of a target Segment. */
    uint64_t cost;
    if(!strncmp(field,"ffn.experts.",12)) {
        uint64_t eid; const char *kind=lmb_plan_uint(field+12,&eid);
        if(!kind || *kind++!='.' || eid>=m->experts) return -1;
        static const char *const names[]={"w1.weight","w1.scale","w2.weight","w2.scale","w3.weight","w3.scale"};
        unsigned i; for(i=0;i<6;i++) if(!strcmp(kind,names[i])) break;
        if(i==6) return -1;
        uint64_t rows=i/2==1 ? v->h : v->inter, cols=i/2==1 ? v->inter : v->h;
        LmbV4Spec s={.dtype=i%2 ? "F8_E8M0" : "I8",.rows=rows,.cols=cols/(i%2 ? 32 : 2)};
        LmbV4Expert *e=&v->expert[layer*m->experts+eid];
        if(!lmb_v4_shape(t,&s) || (e->seen&(1u<<i)) || (e->seen && e->file!=t->file_id)) return -1;
        e->file=t->file_id; e->seen|=1u<<i; e->offset[i]=t->offset;
        cost=t->bytes; /* native expert scales remain encoded UE8M0 */
    } else {
        LmbV4Spec specs[48]; unsigned count=lmb_v4_specs(v,(uint32_t)layer,specs),i;
        for(i=0;i<count;i++) if(!strcmp(field,specs[i].name)) break;
        if(i==count) return 0;
        if((v->seen[layer]&(UINT64_C(1)<<i)) || !lmb_v4_shape(t,&specs[i])) return -1;
        v->seen[layer]|=UINT64_C(1)<<i;
        cost=!strcmp(t->dtype,"F8_E8M0") ? lmb_size_mul(t->elements,4) : t->bytes;
    }
    if(cost>v->largest) v->largest=cost;
    m->memory[layer].resident_bytes=lmb_size_add(m->memory[layer].resident_bytes,cost);
    return m->memory[layer].resident_bytes==UINT64_MAX ? -1 : 0;
}
static int lmb_v4_string(const char *cfg,const char *key,const char *expected) {
    char text[64]; return lmb_json_string(cfg,key,text,sizeof text) || strcmp(text,expected);
}
static int lmb_v4_memory(const char *root,const char *cfg,LmbModelShape *m) {
    LmbV4Inventory v={.m=m}; uint32_t layers,experts,k,vocab,shared,rope,ctx,mtp,iters;
#define V4_NEED(key,dst,lo,hi) if(lmb_plan_u32(cfg,key,lo,hi,&dst)) return -1
    V4_NEED("hidden_size",v.h,128,1048576); V4_NEED("num_hidden_layers",layers,1,128);
    V4_NEED("num_attention_heads",v.heads,1,1024); V4_NEED("head_dim",v.hd,2,65536);
    V4_NEED("q_lora_rank",v.qr,128,1048576); V4_NEED("qk_rope_head_dim",rope,2,65536);
    V4_NEED("o_groups",v.og,1,v.heads); V4_NEED("o_lora_rank",v.orr,1,1048576);
    V4_NEED("sliding_window",v.window,1,1048576); V4_NEED("index_n_heads",v.ih,1,1024);
    V4_NEED("index_head_dim",v.id,2,65536); V4_NEED("index_topk",v.topk,1,1048576);
    V4_NEED("n_routed_experts",experts,1,4096); V4_NEED("num_experts_per_tok",k,1,256);
    V4_NEED("moe_intermediate_size",v.inter,128,16777216); V4_NEED("n_shared_experts",shared,1,1);
    V4_NEED("num_hash_layers",v.hash,0,layers); V4_NEED("hc_mult",v.hc,1,16);
    V4_NEED("vocab_size",vocab,1,16777216); V4_NEED("max_position_embeddings",ctx,4,1048576);
    V4_NEED("num_nextn_predict_layers",mtp,0,16); V4_NEED("hc_sinkhorn_iters",iters,1,1024);
#undef V4_NEED
    (void)shared;(void)mtp;(void)iters;
    if(v.h!=m->hidden || layers!=m->layers || vocab!=m->vocab || k>experts ||
       v.h%128 || v.inter%128 || v.qr%128 || v.heads%v.og || rope%2 || rope>v.hd || rope>v.id ||
       ((uint64_t)(v.heads/v.og)*v.hd)%128 || ((uint64_t)v.og*v.orr)%128 ||
       lmb_v4_string(cfg,"expert_dtype","fp4") || lmb_v4_string(cfg,"scoring_func","sqrtsoftplus") ||
       lmb_v4_string(cfg,"topk_method","noaux_tc")) return -1;
    const char *quant=lmb_json_member(cfg,"quantization_config");
    if(!quant || *quant!='{' || lmb_v4_string(quant,"fmt","e4m3") || lmb_v4_string(quant,"scale_fmt","ue8m0")) return -1;
    static const char *const real_fields[]={"rms_norm_eps","hc_eps","routed_scaling_factor",
        "swiglu_limit","rope_theta","compress_rope_theta"};
    for(unsigned i=0;i<sizeof real_fields/sizeof *real_fields;i++) {
        double value;
        if(!lmb_json_member(cfg,real_fields[i]) || lmb_plan_number(cfg,real_fields[i],0,&value) ||
           value>FLT_MAX || (i==3 ? value<0 : (float)value<=0)) return -1;
    }
    const char *rp=lmb_json_member(cfg,"rope_scaling");
    uint32_t orig,fast,slow; double factor;
    if(!rp || *rp!='{' || lmb_plan_u32(rp,"original_max_position_embeddings",1,1048576,&orig) ||
       lmb_plan_u32(rp,"beta_fast",1,INT32_MAX,&fast) || lmb_plan_u32(rp,"beta_slow",1,INT32_MAX,&slow) ||
       !lmb_json_member(rp,"factor") || lmb_plan_number(rp,"factor",0,&factor) || factor>FLT_MAX ||
       (float)factor<=0 || fast<slow) return -1;
    unsigned nr; if(lmb_plan_array(lmb_json_member(cfg,"compress_ratios"),v.ratios,128,&nr) || nr<layers) return -1;
    for(unsigned i=0;i<nr;i++) if(v.ratios[i]!=0 && v.ratios[i]!=4 && v.ratios[i]!=8 && v.ratios[i]!=128) return -1;
    m->experts=experts;m->experts_per_tok=k;m->moe_intermediate=v.inter;
    v.expert=calloc((size_t)layers*experts,sizeof *v.expert);
    if(!v.expert) return -1;
    int rc=lmb_plan_tensors(root,lmb_v4_tensor,&v);
    if(v.edge!=63) rc=-1;
    uint64_t cells=(uint64_t)v.h*v.inter;
    for(uint32_t i=0;i<layers && !rc;i++) {
        LmbV4Spec specs[48];unsigned n=lmb_v4_specs(&v,i,specs);
        if(v.seen[i]!=(UINT64_C(1)<<n)-1) {rc=-1;break;}
        for(uint32_t e=0;e<experts;e++) {
            LmbV4Expert *r=&v.expert[i*experts+e];
            if(r->seen!=63) {rc=-1;break;}
            /* Native store reads three matrices/scales as two contiguous
             * ranges in one shard. Equal sizes alone do not prove loadability. */
            for(unsigned bank=0;bank<2;bank++) {
                uint64_t a[3]={r->offset[bank],r->offset[bank+2],r->offset[bank+4]};
                for(unsigned x=0;x<3;x++) for(unsigned y=x+1;y<3;y++) if(a[y]<a[x]) {
                    uint64_t swap=a[x];a[x]=a[y];a[y]=swap;
                }
                uint64_t bytes=cells/(bank ? 32 : 2);
                if(a[1]-a[0]!=bytes || a[2]-a[1]!=bytes) rc=-1;
            }
        }
        LmbLayerMemory *l=&m->memory[i];uint64_t ratio=v.ratios[i];
        l->state_fixed_bytes=(uint64_t)v.window*v.hd*4+4096;
        if(ratio) {
            uint64_t overlap=ratio==4 ? 2 : 1;
            l->state_fixed_bytes=lmb_size_add(l->state_fixed_bytes,
                ratio*overlap*overlap*v.hd*8+(uint64_t)16*v.hd*4);
            /* Geometric cache growth and temporary snapshot/reallocation. */
            l->state_token_bytes=(uint64_t)v.hd*16/ratio+16;
        }
        if(ratio==4) {
            uint64_t wide=(uint64_t)v.ih*v.id+v.ih+v.qr+v.id;
            l->state_fixed_bytes=lmb_size_add(l->state_fixed_bytes,
                (uint64_t)128*v.id*4+(uint64_t)128*v.id+65536+wide*128*16);
            /* Persistent per-session batch indexer scratch grows with count. */
            l->state_token_bytes=lmb_size_add(l->state_token_bytes,(uint64_t)v.id*4+256);
        }
        l->state_fixed_bytes=lmb_size_mul(l->state_fixed_bytes,2);
    }
    free(v.expert);if(rc) return -1;
    uint64_t record=cells*3/2+cells*3/32;
    uint64_t wide=(uint64_t)v.h*v.hc+(uint64_t)v.heads*v.hd*3+v.qr+(uint64_t)v.og*v.orr+
        (uint64_t)k*(v.h+v.inter)+experts+(uint64_t)v.ih*v.id;
    m->scratch_fixed_bytes=lmb_size_add(UINT64_C(512)<<20,lmb_size_mul(wide,128u*16u*4u));
    m->scratch_fixed_bytes=lmb_size_add(m->scratch_fixed_bytes,lmb_size_add(v.largest*2,
        lmb_size_mul(record,experts<256 ? experts : 256)));
    m->scratch_token_bytes=(uint64_t)v.hd*4+256u*4u;
    m->segment_fixed_bytes=(uint64_t)layers*(8192u+(uint64_t)experts*2048u);
    m->session_fixed_bytes=(uint64_t)layers*16;
    m->edge_scratch_fixed_bytes=(uint64_t)m->vocab*8+(uint64_t)v.h*128*8;
    m->boundary_width=v.h*v.hc;m->max_context=ctx;m->memory_contract=1;
    return m->scratch_fixed_bytes==UINT64_MAX ? -1 : 0;
}
#endif
