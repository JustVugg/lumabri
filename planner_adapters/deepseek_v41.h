/* Lumabri's resident, single-row, text-only V4.1 contract. This describes the
 * pinned build copy, not Colibri's disk-streaming CLI. No weight payload is
 * read here. Engram hashes and table extents are checked before model_load. */
#ifndef LMB_PLAN_V41_H
#define LMB_PLAN_V41_H
typedef struct {
    uint32_t h,layers,heads,hd,rope,q,o,groups,inter,experts,k,vocab,ctx;
    uint32_t hc,window,ih,id,topk,blocks,block_size,saved;
    uint32_t en,eh,ed,ng,pad;
    uint64_t ratio[64],el[8],erows[8];
    uint8_t kv[64],ix[64];
    uint64_t seen[64],edge,metadata,largest;
    uint8_t *expert_seen;
    LmbModelShape *m;
} LmbV41Inventory;
typedef struct { const char *name,*dtype;uint64_t rows,cols;int widen; } LmbV41Spec;

static int lmb_v41_u32(const char *j,const char *a,const char *alias,
                      uint32_t lo,uint32_t hi,uint32_t *out) {
    const char *key=lmb_json_member(j,a) ? a : alias;
    if(!key || lmb_plan_u32(j,key,lo,hi,out))return -1;
    if(alias && lmb_json_member(j,a) && lmb_json_member(j,alias)) {
        uint32_t other;if(lmb_plan_u32(j,alias,lo,hi,&other) || other!=*out)return -1;
    }
    return 0;
}
static char *lmb_v41_read_json(const char *root,const char *name,size_t cap,uint64_t *bytes) {
    char path[1024];int n=snprintf(path,sizeof path,"%s/%s",root,name);
    if(n<0 || (size_t)n>=sizeof path)return NULL;
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return NULL;
    struct stat st;char *s=NULL;
    if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=2 && (uint64_t)st.st_size<=cap) {
        s=malloc((size_t)st.st_size+1);
        if(s) {
            if(lmb_plan_read_at(fd,s,(size_t)st.st_size,0) || memchr(s,0,(size_t)st.st_size)) {free(s);s=NULL;}
            else {
                s[st.st_size]=0;const char *end=lmb_plan_object_end(lmb_plan_space(s));
                if(!end || *lmb_plan_space(end)){free(s);s=NULL;}
                else if(bytes)*bytes=(uint64_t)st.st_size;
            }
        }
    }
    close(fd);return s;
}
/* Iterating a known-shape array avoids a DOM containing millions of token-map
 * objects. The sidecar's decimal multipliers must stay integers, not doubles. */
static int lmb_v41_punct(const char **p,char ch) {
    *p=lmb_plan_space(*p);if(**p!=ch)return -1;(*p)++;return 0;
}
static int lmb_v41_uint(const char **p,uint64_t *n) {
    const char *end=lmb_plan_uint(lmb_plan_space(*p),n);if(!end)return -1;*p=end;return 0;
}
static int lmb_v41_sidecar(const char *root,const char *cfg,LmbV41Inventory *v) {
    const char *ids=lmb_json_member(cfg,"engram_layer_ids");unsigned count=0;
    if(!ids || lmb_plan_array(ids,v->el,4,&count))return -1;
    v->en=count;
    uint64_t bytes=0;char *j=lmb_v41_read_json(root,"dsv41_engram.json",32u<<20,&bytes);
    if(!count) { /* Never ignore a sidecar containing unconfigured tables. */
        if(j){free(j);return -1;}
        return errno==ENOENT ? 0 : -1;
    }
    if(!j)return -1;
    int rc=-1;uint64_t declared[8];unsigned n;
    if(lmb_plan_array(lmb_json_member(j,"layer_ids"),declared,8,&n)||n!=count)goto done;
    for(unsigned i=0;i<count;i++) {
        if(v->el[i]>=v->layers || v->el[i]!=declared[i])goto done;
        for(unsigned k=0;k<i;k++)if(v->el[k]==v->el[i])goto done;
    }
    if(lmb_v41_u32(j,"max_ngram_size",NULL,2,8,&v->ng)||
       lmb_v41_u32(j,"n_heads",NULL,1,16,&v->eh)||
       lmb_v41_u32(j,"head_dim",NULL,32,512,&v->ed)||v->ed%32||
       lmb_v41_u32(j,"pad_id",NULL,0,INT32_MAX,&v->pad))goto done;
    const char *keys[]={"engram_max_ngram_size","engram_n_heads","engram_head_dim","engram_pad_id"};
    uint32_t values[]={v->ng,v->eh,v->ed,v->pad};
    for(unsigned i=0;i<4;i++){uint32_t x;if(lmb_v41_u32(cfg,keys[i],NULL,0,INT32_MAX,&x)||x!=values[i])goto done;}
    uint64_t primes[4][112]={{0}},offsets[4][112]={{0}},max_token=v->pad;
    const char *p=lmb_json_member(j,"primes");if(!p || lmb_v41_punct(&p,'['))goto done;
    for(unsigned t=0;t<count;t++) {
        if((t && lmb_v41_punct(&p,','))||lmb_v41_punct(&p,'['))goto done;
        for(unsigned g=0;g<v->ng-1;g++) {
            if((g && lmb_v41_punct(&p,','))||lmb_v41_punct(&p,'['))goto done;
            for(unsigned h=0;h<v->eh;h++) {
                uint64_t *x=&primes[t][g*v->eh+h];
                if((h && lmb_v41_punct(&p,','))||lmb_v41_uint(&p,x)||!*x||*x>INT32_MAX)goto done;
            }
            if(lmb_v41_punct(&p,']'))goto done;
        }
        if(lmb_v41_punct(&p,']'))goto done;
    }
    if(lmb_v41_punct(&p,']'))goto done;
    p=lmb_json_member(j,"offsets");if(!p||lmb_v41_punct(&p,'['))goto done;
    unsigned cols=(v->ng-1)*v->eh;
    for(unsigned t=0;t<count;t++) {
        if((t && lmb_v41_punct(&p,','))||lmb_v41_punct(&p,'['))goto done;
        for(unsigned i=0;i<cols;i++) {
            uint64_t *x=&offsets[t][i];
            if((i && lmb_v41_punct(&p,','))||lmb_v41_uint(&p,x)||*x>INT64_MAX-primes[t][i])goto done;
            uint64_t end=*x+primes[t][i];if(end>v->erows[t])v->erows[t]=end;
        }
        if(lmb_v41_punct(&p,']'))goto done;
    }
    if(lmb_v41_punct(&p,']'))goto done;
    p=lmb_json_member(j,"token_map");if(!p||lmb_v41_punct(&p,'['))goto done;
    for(uint32_t i=0;i<v->vocab;i++) {
        uint64_t x;if((i && lmb_v41_punct(&p,','))||lmb_v41_uint(&p,&x)||x>INT32_MAX)goto done;
        if(x>max_token)max_token=x;
    }
    if(lmb_v41_punct(&p,']'))goto done;
    p=lmb_json_member(j,"multipliers");if(!p||lmb_v41_punct(&p,'['))goto done;
    for(unsigned t=0;t<count;t++) {
        if((t && lmb_v41_punct(&p,','))||lmb_v41_punct(&p,'['))goto done;
        for(unsigned i=0;i<v->ng;i++) {
            uint64_t x;
            if((i && lmb_v41_punct(&p,','))||lmb_v41_punct(&p,'"')||lmb_v41_uint(&p,&x)||
               lmb_v41_punct(&p,'"')||!x||x>(uint64_t)INT64_MAX/(max_token ? max_token : 1))goto done;
        }
        if(lmb_v41_punct(&p,']'))goto done;
    }
    if(lmb_v41_punct(&p,']'))goto done;
    v->metadata=lmb_size_add(v->metadata,lmb_size_mul(bytes,64));rc=0;
done:free(j);return rc;
}

static unsigned lmb_v41_specs(const LmbV41Inventory *v,uint32_t l,LmbV41Spec *s) {
    unsigned n=0;uint64_t H=v->h,D=v->hd,HC=v->hc,M=(HC+2)*HC;
#define S(name,dt,r,c,w) do{s[n++]=(LmbV41Spec){name,dt,r,c,w};}while(0)
#define F(name,r,c) S(name ".weight","F8_E4M3",r,c,0);S(name ".scale","F8_E8M0",((r)+31)/32,((c)+31)/32,0)
#define W(name,r,c) S(name,"float",r,c,1)
    F("attn.wq_a",v->q,H);F("attn.wq_b",(uint64_t)v->heads*D,v->q);F("attn.wkv",D,H);
    F("attn.wo_a",(uint64_t)v->groups*v->o,(uint64_t)v->heads*D/v->groups);
    F("attn.wo_b",H,(uint64_t)v->groups*v->o);
    W("attn.q_norm.weight",v->q,0);W("attn.kv_norm.weight",D,0);W("attn.attn_sink",v->heads,0);
    W("attn_norm.weight",H,0);W("ffn_norm.weight",H,0);
    W("hc_attn_fn",M,HC*H);W("hc_ffn_fn",M,HC*H);
    W("hc_attn_base",M,0);W("hc_ffn_base",M,0);W("hc_attn_scale",3,0);W("hc_ffn_scale",3,0);
    S("ffn.gate.weight","BF16",v->experts,H,0);W("ffn.gate.bias",v->experts,0);
    F("ffn.shared_experts.w1",v->inter,H);F("ffn.shared_experts.w3",v->inter,H);F("ffn.shared_experts.w2",H,v->inter);
    if(v->kv[l]) {
        S("attn.compressor.wkv.weight","BF16",D,H,0);W("attn.compressor.norm.weight",D,0);
        if(v->ratio[l]>1){S("attn.compressor.wgate.weight","BF16",D,H,0);}
        S("attn.indexer.wk.weight","BF16",v->id,D,0);W("attn.indexer.k_norm.weight",v->id,0);
    }
    if(v->ix[l]) {
        F("attn.indexer.wq_b",(uint64_t)v->ih*v->id,v->q);
        S("attn.indexer.weights_proj.weight","BF16",v->ih,H,0);
    }
    for(unsigned e=0;e<v->en;e++)if(v->el[e]==l) {
        F("engram.wkv",H*(HC+1),(uint64_t)(v->ng-1)*v->eh*v->ed);
        W("engram.q_weight",HC,H);W("engram.k_weight",HC,H);
        S("engram.embed.weight","F8_E4M3",v->erows[e],v->ed,0);
        S("engram.embed.scale","F8_E8M0",v->erows[e],v->ed/32,0);
    }
#undef W
#undef F
#undef S
    return n;
}
static int lmb_v41_tensor_shape(const LmbPlanTensor *t,const LmbV41Spec *s) {
    int dtype=s->widen ? (!strcmp(t->dtype,"F32")||!strcmp(t->dtype,"BF16")||!strcmp(t->dtype,"F16")) :
        (!strcmp(t->dtype,s->dtype)||(!strcmp(s->dtype,"F8_E8M0")&&!strcmp(t->dtype,"U8")));
    return dtype && t->rank==(s->cols?2u:1u) && t->shape[0]==s->rows && (!s->cols||t->shape[1]==s->cols);
}
static int lmb_v41_tensor(const LmbPlanTensor *t,void *opaque) {
    LmbV41Inventory *v=opaque;LmbModelShape *m=v->m;
    v->metadata=lmb_size_add(v->metadata,2048+strlen(t->name)*32);
    const LmbV41Spec globals[]={{"embed.weight","BF16",v->vocab,v->h,0},
        {"head.weight","BF16",v->vocab,v->h,0},{"norm.weight","float",v->h,0,1}};
    for(unsigned i=0;i<3;i++)if(!strcmp(t->name,globals[i].name)) {
        if((v->edge&(1u<<i))||!lmb_v41_tensor_shape(t,&globals[i]))return -1;
        v->edge|=1u<<i;m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,i==2?t->elements*4:t->bytes);return 0;
    }
    if(strncmp(t->name,"layers.",7))return 0; /* vision and draft towers are not text weights */
    uint64_t l;const char *field=lmb_plan_uint(t->name+7,&l);if(!field||*field++!='.'||l>=v->layers)return -1;
    uint64_t cost=0;
    if(!strncmp(field,"ffn.experts.",12)) {
        uint64_t eid;const char *kind=lmb_plan_uint(field+12,&eid);if(!kind||*kind++!='.'||eid>=v->experts)return -1;
        static const char *names[]={"w1.weight","w1.scale","w3.weight","w3.scale","w2.weight","w2.scale"};
        unsigned i;for(i=0;i<6;i++)if(!strcmp(kind,names[i]))break;if(i==6)return -1;
        uint64_t r=i>=4?v->h:v->inter,c=i>=4?v->inter:v->h;
        LmbV41Spec s={NULL,i%2?"F8_E8M0":"I8",r,c/(i%2?32:2),0};
        uint8_t *seen=&v->expert_seen[l*v->experts+eid];
        if((*seen&(1u<<i))||!lmb_v41_tensor_shape(t,&s))return -1;
        *seen|=1u<<i;cost=lmb_size_add(t->bytes,8192); /* aligned payload + read slack */
    } else {
        LmbV41Spec s[64];unsigned n=lmb_v41_specs(v,(uint32_t)l,s),i;
        for(i=0;i<n;i++)if(!strcmp(field,s[i].name))break;
        if(i==n || (v->seen[l]&(UINT64_C(1)<<i)) || !lmb_v41_tensor_shape(t,&s[i]))return -1;
        v->seen[l]|=UINT64_C(1)<<i;cost=s[i].widen?lmb_size_mul(t->elements,4):t->bytes;
    }
    if(cost>v->largest)v->largest=cost;
    m->memory[l].resident_bytes=lmb_size_add(m->memory[l].resident_bytes,cost);
    return m->memory[l].resident_bytes==UINT64_MAX?-1:0;
}

static int lmb_v41_memory(const char *root,const char *cfg,LmbModelShape *m) {
    LmbV41Inventory v={.m=m};uint32_t shared,source,iters;
#define N(a,b,d,lo,hi) if(lmb_v41_u32(cfg,a,b,lo,hi,&d))return -1
    N("dim","hidden_size",v.h,32,65536);N("n_layers","num_hidden_layers",v.layers,1,64);
    N("n_heads","num_attention_heads",v.heads,1,1024);N("head_dim",NULL,v.hd,32,512);
    N("rope_head_dim","qk_rope_head_dim",v.rope,2,v.hd);N("q_lora_rank",NULL,v.q,32,65536);
    N("o_lora_rank",NULL,v.o,1,65536);N("o_groups",NULL,v.groups,1,v.heads);
    N("moe_inter_dim","moe_intermediate_size",v.inter,32,1048576);N("n_routed_experts",NULL,v.experts,1,4096);
    N("n_activated_experts","num_experts_per_tok",v.k,1,256);N("n_shared_experts",NULL,shared,1,1);
    N("vocab_size",NULL,v.vocab,1,16777216);N("max_seq_len","max_position_embeddings",v.ctx,2,1048576);
    N("hc_mult",NULL,v.hc,1,16);N("hc_sinkhorn_iters",NULL,iters,1,1024);
    N("window_size","sliding_window",v.window,1,1048576);N("index_n_heads",NULL,v.ih,1,1024);
    N("index_head_dim",NULL,v.id,2,512);N("index_topk",NULL,v.topk,1,1048576);
    N("candidate_topk_blocks",NULL,v.blocks,1,1048576);N("candidate_block_size",NULL,v.block_size,1,1048576);
    N("candidate_source_layer","candidate_source_layer_id",source,0,v.layers-1);
    v.saved=1;if(lmb_json_member(cfg,"dspark_block_size")){N("dspark_block_size",NULL,v.saved,0,64);v.saved++;}
#undef N
    (void)shared;(void)iters;
    if(v.h%32||v.inter%32||v.o%32||v.k>v.experts||v.heads%v.groups||v.rope%2||v.rope>v.id)return -1;
    unsigned n;if(lmb_plan_array(lmb_json_member(cfg,"compress_ratios"),v.ratio,64,&n)||n!=v.layers)return -1;
    for(unsigned i=0;i<n;i++)if(v.ratio[i]>v.ctx)return -1;
    const char *list[2]={"kv_source_layers","index_source_layers"},*alias[2]={"kv_source_layer_ids","index_source_layer_ids"};
    for(unsigned which=0;which<2;which++) {
        uint64_t ids[64];const char *p=lmb_json_member(cfg,list[which]);if(!p)p=lmb_json_member(cfg,alias[which]);
        if(lmb_plan_array(p,ids,64,&n)||!n)return -1;
        for(unsigned j=0;j<n;j++) {
            if(ids[j]>=v.layers)return -1;
            uint8_t *slot=which?&v.ix[ids[j]]:&v.kv[ids[j]];
            if(*slot)return -1;
            *slot=1;if(!which && !v.ratio[ids[j]])return -1;
        }
    }
    if(!v.ix[source])return -1;
    static const char *const floats[]={"norm_eps","gate_temp","route_scale","hc_eps","rope_theta","compress_rope_theta","rope_factor"};
    for(unsigned i=0;i<sizeof floats/sizeof *floats;i++) {
        double x;if(!lmb_json_member(cfg,floats[i])||lmb_plan_number(cfg,floats[i],0,&x)||x<=0||x>FLT_MAX)return -1;
    }
    if(lmb_v41_sidecar(root,cfg,&v))return -1;
    m->layers=v.layers;m->hidden=v.h;m->heads=v.heads;m->kv_heads=v.heads;m->vocab=v.vocab;
    m->experts=v.experts;m->experts_per_tok=v.k;m->moe_intermediate=v.inter;m->bits_per_weight=4;
    v.expert_seen=calloc((size_t)v.layers*v.experts,1);if(!v.expert_seen)return -1;
    int rc=lmb_plan_tensors(root,lmb_v41_tensor,&v);
    if(v.edge!=7)rc=-1;
    for(unsigned l=0;l<v.layers && !rc;l++) {
        LmbV41Spec s[64];unsigned count=lmb_v41_specs(&v,l,s);
        if(v.seen[l]!=(UINT64_C(1)<<count)-1)rc=-1;
        for(unsigned e=0;e<v.experts;e++)if(v.expert_seen[l*v.experts+e]!=63)rc=-1;
    }
    free(v.expert_seen);if(rc)return -1;
    /* Both the prepared template and each conversation own state. Count two
     * copies per admitted session (conservative for >1), including ALL foreign
     * compressed KV owners. Weights, rope and token map are shared read-only. */
    uint64_t aux=1+3+v.blocks+2+v.topk,owner_token=0;
    for(unsigned l=0;l<v.layers;l++) {
        if(v.kv[l]) {aux+=1+v.hd+v.id;owner_token+=((uint64_t)(v.hd+v.id)*4+v.ratio[l]-1)/v.ratio[l];}
        uint64_t state=(uint64_t)(v.window+v.saved)*(v.hd+1)*4+4096+(uint64_t)v.experts*256;
        if(v.kv[l] && v.ratio[l]>1)state+=v.ratio[l]*v.hd*8+(uint64_t)v.saved*(v.hd*8+4);
        for(unsigned e=0;e<v.en;e++)if(v.el[e]==l)state+=32*(16+(uint64_t)v.ed*4);
        m->memory[l].state_fixed_bytes=lmb_size_mul(state,2);
    }
    uint64_t width=2+(uint64_t)v.hc*(v.h+1)+aux;if(width>1048576)return -1;
    m->boundary_width=(uint32_t)width;
    m->session_fixed_bytes=2*((UINT64_C(2)<<20)+(uint64_t)v.layers*8192+width*16);
    m->session_token_bytes=2*(owner_token+32+(uint64_t)v.rope*4); /* includes shared rope tables once per admission */
    m->segment_fixed_bytes=lmb_size_add(UINT64_C(16)<<20,lmb_size_add(v.metadata,(uint64_t)v.vocab*4));
    uint64_t wide=(uint64_t)v.h*v.hc*4+(uint64_t)v.heads*v.hd*4+v.q+(uint64_t)v.groups*v.o+
        (uint64_t)v.k*(v.h+v.inter)+(uint64_t)v.ih*v.id+v.experts+width;
    m->scratch_fixed_bytes=lmb_size_add(UINT64_C(16)<<20,lmb_size_add(wide*256,v.largest));
    m->scratch_token_bytes=(uint64_t)(v.hd+v.id+v.ih+32)*16;
    uint64_t tok_bytes=0;char *tok=lmb_v41_read_json(root,"tokenizer.json",64u<<20,&tok_bytes);
    if(!tok)return -1;
    free(tok);
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,lmb_size_add(v.metadata,tok_bytes*64+(uint64_t)v.vocab*256));
    m->edge_scratch_fixed_bytes=(UINT64_C(16)<<20)+(uint64_t)v.vocab*8+(uint64_t)v.h*16+width*16;
    m->max_context=v.ctx;m->memory_contract=1;m->disk_streaming=0;
    return m->segment_fixed_bytes==UINT64_MAX||m->scratch_fixed_bytes==UINT64_MAX||m->edge_resident_bytes==UINT64_MAX?-1:0;
}
#endif
