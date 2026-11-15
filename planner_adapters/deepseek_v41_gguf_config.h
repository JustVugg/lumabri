/* Canonical text configuration and Engram metadata for Colibri 2.1 V4.1.
 * Payloads stay in GGUF. Hash multipliers never pass through floating point. */
#ifndef LMB_V41_GGUF_CONFIG_H
#define LMB_V41_GGUF_CONFIG_H
#include "gguf_inventory.h"
#include "src/planner/lumabri_gguf_json.h"
typedef struct {const char *name,*alias,*key;double fallback;int integer;} LmbV41GField;
static const LmbV41GField LMB_V41G_FIELDS[]={
    {"hidden_size","dim","embedding_length",NAN,1},
    {"num_hidden_layers","n_layers","block_count",NAN,1},
    {"num_attention_heads","n_heads","attention.head_count",NAN,1},
    {"head_dim",NULL,"attention.key_length",NAN,1},
    {"qk_rope_head_dim","rope_head_dim","rope.dimension_count",NAN,1},
    {"q_lora_rank",NULL,"attention.q_lora_rank",NAN,1},
    {"o_lora_rank",NULL,"attention.output_lora_rank",NAN,1},
    {"o_groups",NULL,"attention.output_group_count",1,1},
    {"moe_intermediate_size","moe_inter_dim","expert_feed_forward_length",NAN,1},
    {"n_routed_experts",NULL,"expert_count",NAN,1},
    {"num_experts_per_tok","n_activated_experts","expert_used_count",NAN,1},
    {"n_shared_experts",NULL,"expert_shared_count",1,1},
    {"max_position_embeddings","max_seq_len","context_length",1048576,1},
    {"sliding_window","window_size","attention.sliding_window",128,1},
    {"index_n_heads",NULL,"attention.indexer.head_count",NAN,1},
    {"index_head_dim",NULL,"attention.indexer.key_length",NAN,1},
    {"index_topk",NULL,"attention.indexer.top_k",NAN,1},
    {"candidate_source_layer","candidate_source_layer_id","attention.indexer.candidate_source_layer",NAN,1},
    {"candidate_topk_blocks",NULL,"attention.indexer.candidate_top_k_blocks",NAN,1},
    {"candidate_block_size",NULL,"attention.indexer.candidate_block_size",NAN,1},
    {"hc_mult",NULL,"hyper_connection.count",NAN,1},
    {"hc_sinkhorn_iters",NULL,"hyper_connection.sinkhorn_iterations",20,1},
    {"norm_eps","rms_norm_eps","attention.layer_norm_rms_epsilon",1e-6,0},
    {"hc_eps",NULL,"hyper_connection.epsilon",1e-6,0},
    {"route_scale","routed_scaling_factor","expert_weights_scale",1,0},
    {"gate_temp",NULL,"expert_gate_temperature",1,0},
    {"rope_theta",NULL,"rope.freq_base",10000,0},
    {"compress_rope_theta",NULL,"attention.compress_rope_freq_base",40000,0}
};
static const LmbGgufValue *lmb_v41g_value(const LmbGguf *g,const char *key) {
    char name[128];int n=snprintf(name,sizeof name,"deepseek41.%s",key);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_v41g_number(const LmbGguf *g,const char *cfg,const LmbV41GField *f,double *out) {
    if(cfg) {
        const char *key=lmb_json_member(cfg,f->name)?f->name:f->alias;
        if(!key)key=f->name;
        double fallback=!strcmp(f->name,"max_position_embeddings")?4096:f->fallback;
        if(lmb_plan_number(cfg,key,fallback,out))return -1;
        if(f->alias && lmb_json_member(cfg,f->name) && lmb_json_member(cfg,f->alias)) {
            double other;if(lmb_plan_number(cfg,f->alias,NAN,&other) || other!=*out)return -1;
        }
    } else if(lmb_gguf_number(lmb_v41g_value(g,f->key),f->fallback,out))return -1;
    return !isfinite(*out) || *out<0 || *out>INT32_MAX || (f->integer && floor(*out)!=*out)?-1:0;
}
static int lmb_v41g_array(const LmbGguf *g,const char *cfg,const char *name,const char *alias,
                         const char *key,uint64_t *values,unsigned cap,unsigned *count,int optional) {
    const char *p=cfg?lmb_json_member(cfg,name):NULL;
    if(!p && cfg && alias)p=lmb_json_member(cfg,alias);
    if(cfg) {
        if(!p && optional){*count=0;return 0;}
        return lmb_plan_array(p,values,cap,count);
    }
    const LmbGgufValue *v=lmb_v41g_value(g,key);
    if(!v && optional){*count=0;return 0;}
    if(!v || v->type!=9 || v->count>cap)return -1;
    *count=(unsigned)v->count;
    for(unsigned i=0;i<*count;i++)if(lmb_gguf_inventory_item(v,i,&values[i]))return -1;
    return 0;
}
static void lmb_v41g_json_array(LmbGgufJson *j,const char *key,const uint64_t *v,unsigned n,int strings) {
    lmb_gguf_json_printf(j,",\"%s\":[",key);
    for(unsigned i=0;i<n;i++)lmb_gguf_json_printf(j,strings?"%s\"%llu\"":"%s%llu",i?",":"",(unsigned long long)v[i]);
    lmb_gguf_json_printf(j,"]");
}
static int lmb_v41g_engram(const LmbGguf *g,const char *cfg,LmbGgufJson *j,LmbGgufJson *side,uint32_t vocab) {
    uint64_t ids[4];unsigned nt;
    if(lmb_v41g_array(g,cfg,"engram_layer_ids",NULL,"ple.layers",ids,4,&nt,1))return -1;
    lmb_v41g_json_array(j,"engram_layer_ids",ids,nt,0);
    if(!nt)return 0;
    const LmbV41GField fields[]={
        {"engram_max_ngram_size",NULL,"ple.ngram_size",NAN,1},
        {"engram_n_heads",NULL,"ple.heads_per_ngram",NAN,1},
        {"engram_head_dim",NULL,"embedding_length_per_layer_input",NAN,1},
        {"engram_pad_token_id",NULL,"engram.pad_token_id",2,1}};
    double n[4];for(unsigned i=0;i<4;i++)if(lmb_v41g_number(g,cfg,&fields[i],&n[i]))return -1;
    unsigned ng=(unsigned)n[0],heads=(unsigned)n[1],dim=(unsigned)n[2],pad=(unsigned)n[3];
    if(ng<2 || ng>8 || !heads || heads>16 || dim<32 || dim>512 || dim%32 || pad>=vocab)return -1;
    unsigned per=(ng-1)*heads;uint64_t sizes[448],offsets[448],mult[32],rows[4],end=0;
    const LmbGgufValue *primes=lmb_v41g_value(g,"engram.primes"),*offs=NULL,*rs=NULL;
    const LmbGgufValue *multipliers=lmb_v41g_value(g,primes?"engram.multipliers":"ple.layer_multipliers");
    if(primes) {
        const char *encoding=lmb_gguf_text(lmb_v41g_value(g,"engram.encoding"));
        if(!encoding || strcmp(encoding,"e4m3_e8m0_32_row264"))return -1;
        const LmbGgufValue *layers=lmb_v41g_value(g,"engram.layer_ids");
        rs=lmb_v41g_value(g,"engram.rows");
        if(!layers || layers->count!=nt || !rs || rs->count!=nt)return -1;
        for(unsigned i=0;i<nt;i++){uint64_t v;if(lmb_gguf_inventory_item(layers,i,&v)||v!=ids[i])return -1;}
    } else {primes=lmb_v41g_value(g,"ple.head_vocab_sizes");offs=lmb_v41g_value(g,"ple.head_offsets");}
    if(!primes || primes->count!=nt*per || !multipliers || multipliers->count!=nt*ng ||
       (offs && offs->count!=nt*per) || (!rs && !offs))return -1;
    for(unsigned t=0;t<nt;t++) {
        if(rs)end=0;
        uint64_t start=end;
        for(unsigned i=t*per;i<(t+1)*per;i++) {
            if(lmb_gguf_inventory_item(primes,i,&sizes[i]) || !sizes[i] || sizes[i]>INT32_MAX)return -1;
            offsets[i]=end;
            uint64_t off;if(offs && (lmb_gguf_inventory_item(offs,i,&off)||off!=end))return -1;
            end=lmb_size_add(end,sizes[i]);if(end>INT64_MAX)return -1;
        }
        rows[t]=end-start;
        uint64_t r;if(rs && (lmb_gguf_inventory_item(rs,t,&r)||r!=rows[t]))return -1;
    }
    for(unsigned i=0;i<nt*ng;i++)if(lmb_gguf_inventory_item(multipliers,i,&mult[i]) || !mult[i] || mult[i]>INT64_MAX)return -1;
    const LmbGgufValue *map=lmb_v41g_value(g,"engram.token_map");uint64_t mapped_pad;
    if(!map || map->count!=vocab || lmb_gguf_inventory_item(map,pad,&mapped_pad) || mapped_pad>INT32_MAX)return -1;
    lmb_gguf_json_printf(j,",\"engram_max_ngram_size\":%u,\"engram_n_heads\":%u,\"engram_head_dim\":%u,\"engram_pad_token_id\":%u,\"engram_pad_id\":%llu",
        ng,heads,dim,pad,(unsigned long long)mapped_pad);
    lmb_gguf_json_printf(side,"{\"max_ngram_size\":%u,\"n_heads\":%u,\"head_dim\":%u,\"pad_id\":%llu",ng,heads,dim,(unsigned long long)mapped_pad);
    lmb_v41g_json_array(side,"layer_ids",ids,nt,0);lmb_v41g_json_array(side,"num_embeddings",rows,nt,0);
    lmb_gguf_json_printf(side,",\"primes\":[");
    for(unsigned t=0;t<nt;t++) {
        lmb_gguf_json_printf(side,"%s[",t?",":"");
        for(unsigned a=0;a<ng-1;a++) {
            lmb_gguf_json_printf(side,"%s[",a?",":"");
            for(unsigned h=0;h<heads;h++)lmb_gguf_json_printf(side,"%s%llu",h?",":"",(unsigned long long)sizes[t*per+a*heads+h]);
            lmb_gguf_json_printf(side,"]");
        }
        lmb_gguf_json_printf(side,"]");
    }
    lmb_gguf_json_printf(side,"],\"offsets\":[");
    for(unsigned t=0;t<nt;t++) {
        lmb_gguf_json_printf(side,"%s[",t?",":"");
        for(unsigned i=0;i<per;i++)lmb_gguf_json_printf(side,"%s%llu",i?",":"",(unsigned long long)offsets[t*per+i]);
        lmb_gguf_json_printf(side,"]");
    }
    lmb_gguf_json_printf(side,"],\"multipliers\":[");
    for(unsigned t=0;t<nt;t++) {
        lmb_gguf_json_printf(side,"%s[",t?",":"");
        for(unsigned i=0;i<ng;i++)lmb_gguf_json_printf(side,"%s\"%llu\"",i?",":"",(unsigned long long)mult[t*ng+i]);
        lmb_gguf_json_printf(side,"]");
    }
    lmb_gguf_json_printf(side,"],\"token_map\":[");
    uint64_t largest=mapped_pad;
    for(uint32_t i=0;i<vocab;i++) {
        uint64_t v;if(lmb_gguf_inventory_item(map,i,&v) || v>INT32_MAX)return -1;
        if(v>largest)largest=v;
        lmb_gguf_json_printf(side,"%s%llu",i?",":"",(unsigned long long)v);
    }
    for(unsigned i=0;i<nt*ng;i++)if(mult[i]>(uint64_t)INT64_MAX/(largest?largest:1))return -1;
    lmb_gguf_json_printf(side,"]}");return j->bad || side->bad?-1:0;
}
static int lmb_v41g_config(const LmbGguf *g,const char *override,LmbGgufJson *j,LmbGgufJson *side) {
    const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"deepseek41"))return -1;
    const LmbGgufValue *raw=lmb_v41g_value(g,"config");const char *cfg=override;
    if(!cfg && raw)cfg=lmb_gguf_text(raw);
    if(raw && !cfg)return -1;
    if(cfg) {
        const char *end=lmb_plan_object_end(lmb_plan_space(cfg));if(!end || (!override && *lmb_plan_space(end)))return -1;
        size_t bytes=(size_t)(end-cfg);if(bytes>65535)return -1;
        LmbJsonToken *tokens=calloc(bytes+1,sizeof *tokens);if(!tokens)return -1;
        LmbJson syntax;int bad=lmb_json_parse(&syntax,cfg,bytes,tokens,(unsigned)bytes+1);free(tokens);if(bad)return -1;
        const char *text=lmb_json_member(cfg,"text_config");if(text)cfg=text;
        const char *scores[]={"score_func","scoring_func"};
        for(unsigned i=0;i<2;i++)if(lmb_json_member(cfg,scores[i])) {
            char score[32];if(lmb_json_string(cfg,scores[i],score,sizeof score) || strcmp(score,"sqrtsoftplus"))return -1;
        }
    }
    const LmbGgufTensor *emb=lmb_gguf_tensor(g,"token_embd.weight");
    if(!emb || emb->rank!=2 || !emb->shape[1] || emb->shape[1]>1048576)return -1;
    uint32_t vocab=(uint32_t)emb->shape[1];uint64_t ids[64];unsigned n;
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!tokens || tokens->count!=vocab)return -1;
    if(cfg) {uint32_t supplied;if(lmb_plan_u32(cfg,"vocab_size",vocab,vocab,&supplied))return -1;}
    lmb_gguf_json_printf(j,"{\"model_type\":\"deepseek_v41\",\"vocab_size\":%u",vocab);
    for(unsigned i=0;i<sizeof LMB_V41G_FIELDS/sizeof *LMB_V41G_FIELDS;i++) {
        double v;const LmbV41GField *f=&LMB_V41G_FIELDS[i];if(lmb_v41g_number(g,cfg,f,&v))return -1;
        lmb_gguf_json_printf(j,",\"%s\":%.17g",f->name,v);
    }
    /* Text execution still allocates saved ring rows from this field even
     * with draft loading disabled. Preserve and price the real allocation. */
    uint32_t saved=0;
    if(cfg && lmb_json_member(cfg,"dspark_block_size") && lmb_plan_u32(cfg,"dspark_block_size",0,64,&saved))return -1;
    lmb_gguf_json_printf(j,",\"dspark_block_size\":%u",saved);
    const char *array_names[]={"compress_ratios","kv_source_layers","index_source_layers"};
    const char *aliases[]={NULL,"kv_source_layer_ids","index_source_layer_ids"};
    const char *keys[]={"attention.compress_ratios","attention.kv_source_layers","attention.indexer.source_layers"};
    for(unsigned i=0;i<3;i++) {
        if(lmb_v41g_array(g,cfg,array_names[i],aliases[i],keys[i],ids,64,&n,0))return -1;
        lmb_v41g_json_array(j,array_names[i],ids,n,0);
    }
    const char *rp=cfg?lmb_json_member(cfg,"rope_scaling"):NULL;
    const LmbV41GField rope[]={
        {"factor",NULL,"rope.scaling.factor",1,0},
        {"original_max_position_embeddings",NULL,"rope.scaling.original_context_length",0,1},
        {"beta_fast",NULL,"rope.scaling.yarn_beta_fast",32,1},
        {"beta_slow",NULL,"rope.scaling.yarn_beta_slow",1,1}};
    const char *flat[]={"rope_factor","original_seq_len","beta_fast","beta_slow"};
    for(unsigned i=0;i<4;i++) {
        double v;LmbV41GField f=rope[i];
        if(cfg) {
            f.name=flat[i];if(lmb_v41g_number(g,cfg,&f,&v))return -1;
            if(rp) {f.name=rope[i].name;f.fallback=v;if(lmb_v41g_number(g,rp,&f,&v))return -1;}
        } else {
            if(i==1 && lmb_gguf_number(lmb_v41g_value(g,"context_length"),65536,&f.fallback))return -1;
            if(lmb_v41g_number(g,NULL,&f,&v))return -1;
        }
        lmb_gguf_json_printf(j,",\"%s\":%.17g",flat[i],v);
    }
    double clamp=0;
    if(cfg) {if(lmb_plan_number(cfg,"swiglu_limit",0,&clamp))return -1;}
    else {
        const LmbGgufValue *v=lmb_v41g_value(g,"swiglu_clamp_exp");
        if(v) {
            unsigned width=lmb_gguf_scalar_bytes(v->element_type);
            if(v->type!=9 || !v->count || !width || width*v->count!=v->bytes)return -1;
            for(uint32_t i=0;i<v->count;i++) {
                LmbGgufValue x={.type=v->element_type,.bytes=width,.data=v->data+(size_t)i*width};double z;
                if(lmb_gguf_number(&x,NAN,&z) || (i && z!=clamp))return -1;
                clamp=z;
            }
        } else clamp=10;
    }
    if(!isfinite(clamp) || clamp<0 || clamp>FLT_MAX)return -1;
    uint64_t norm=1;int norm_boolean=!cfg;
    if(cfg) {
        const char *value=lmb_json_member(cfg,"norm_topk_prob");
        if(value) {
            if(!strncmp(value,"true",4)){norm=1;norm_boolean=1;}
            else if(!strncmp(value,"false",5)){norm=0;norm_boolean=1;}
            else if(!lmb_plan_uint(value,&norm) || norm>1)return -1;
        }
    } else {
        const LmbGgufValue *v=lmb_v41g_value(g,"expert_weights_norm");
        if(v && (lmb_gguf_integer(v,&norm) || norm>1))return -1;
        v=lmb_v41g_value(g,"expert_gating_func");uint64_t gating;
        if(v && (lmb_gguf_integer(v,&gating) || gating!=4))return -1;
    }
    /* Preserve the importer's JSON type as well as the value. The pinned
     * loader's numeric-only jnum treats booleans differently from 0/1. */
    lmb_gguf_json_printf(j,",\"norm_topk_prob\":%s,\"swiglu_limit\":%.17g",
        norm_boolean?(norm?"true":"false"):(norm?"1":"0"),clamp);
    if(lmb_v41g_engram(g,cfg,j,side,vocab))return -1;
    lmb_gguf_json_printf(j,"}");return j->bad || side->bad?-1:0;
}
#endif
