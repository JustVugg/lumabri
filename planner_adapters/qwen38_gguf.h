#ifndef LMB_PLAN_QWEN38_GGUF_H
#define LMB_PLAN_QWEN38_GGUF_H
#include "gguf_inventory.h"
typedef struct {
    uint32_t layers,h,vocab,context,eos,hc,rank,qh,kv,hd,rot,ih,id,budget,ratio;
    uint32_t experts,topk,inter,shared,kh,vh,kd,vd,conv,cd,ple,nd,nh,pd,pc;
    uint8_t full[LMB_PLAN_LAYER_MAX];double eps,theta;
} LmbQ38Gguf;
static const LmbGgufValue *lmb_q38g_value(const LmbGguf *g,const char *key) {
    char name[128];int n=snprintf(name,sizeof name,"qwen4exp.%s",key);
    return n>0 && (size_t)n<sizeof name?lmb_gguf_get(g,name):NULL;
}
static int lmb_q38g_uint(const LmbGguf *g,const char *key,uint32_t def,uint32_t lo,uint32_t hi,uint32_t *out) {
    const LmbGgufValue *v=lmb_q38g_value(g,key);uint64_t n=def;
    if((v && lmb_gguf_integer(v,&n)) || n<lo || n>hi)return -1;
    *out=(uint32_t)n;return 0;
}
static int lmb_q38g_geometry(const LmbGguf *g,LmbQ38Gguf *c) {
    memset(c,0,sizeof *c);const char *arch=lmb_gguf_text(lmb_gguf_get(g,"general.architecture"));
    if(!arch || strcmp(arch,"qwen4exp"))return -1;
    uint32_t mtp,inner,ngram,per;
#define QU(k,f,d,lo,hi) if(lmb_q38g_uint(g,k,d,lo,hi,&c->f))return -1
    QU("block_count",layers,0,1,LMB_PLAN_LAYER_MAX);QU("embedding_length",h,0,1,65536);
    QU("context_length",context,262144,1,262144);QU("hyper_connection.count",hc,0,2,16);
    QU("hyper_connection.low_rank",rank,0,1,65536);QU("attention.head_count",qh,0,1,65536);
    QU("attention.head_count_kv",kv,0,1,65536);QU("attention.key_length",hd,0,1,65536);
    QU("rope.dimension_count",rot,c->hd,2,c->hd);QU("attention.indexer.head_count",ih,0,1,65536);
    QU("attention.indexer.key_length",id,0,1,65536);QU("attention.indexer.top_k",budget,0,1,262144);
    QU("expert_count",experts,0,1,1024);QU("expert_used_count",topk,0,1,256);
    QU("expert_feed_forward_length",inter,0,1,1048576);
    QU("expert_shared_feed_forward_length",shared,c->inter,1,1048576);
    QU("ssm.group_count",kh,0,1,65536);QU("ssm.time_step_rank",vh,0,1,65536);
    QU("ssm.state_size",kd,0,1,65536);QU("ssm.conv_kernel",conv,0,2,65536);
    QU("embedding_length_per_layer_input",nd,0,1,512);QU("ple.conv_kernel",pc,0,2,65536);
#undef QU
    if(lmb_q38g_uint(g,"nextn_predict_layers",0,0,0,&mtp) ||
       lmb_q38g_uint(g,"ssm.inner_size",0,1,1048576,&inner) ||
       lmb_q38g_uint(g,"ple.ngram_size",0,3,3,&ngram) ||
       lmb_q38g_uint(g,"ple.heads_per_ngram",0,1,32,&per) || inner%c->vh ||
       c->vh%c->kh || c->qh%c->kv || c->topk>c->experts || (c->rot&1) || c->id<c->rot)return -1;
    c->vd=inner/c->vh;c->nh=2*per;c->pd=c->nd*c->nh;
    uint64_t cd=2u*(uint64_t)c->kh*c->kd+inner;
    if(c->vd>512 || c->pd>65536 || cd>INT32_MAX ||
       2u*(uint64_t)c->qh*c->hd>INT32_MAX || (c->ih+1u)*(uint64_t)c->id>INT32_MAX)return -1;
    c->cd=(uint32_t)cd;
    const LmbGgufTensor *embed=lmb_gguf_tensor(g,"token_embd.weight");
    if(!embed || embed->rank!=2 || embed->shape[0]!=c->h || !embed->shape[1] || embed->shape[1]>(1u<<22))return -1;
    c->vocab=(uint32_t)embed->shape[1];uint64_t eos;
    const LmbGgufValue *tokens=lmb_gguf_get(g,"tokenizer.ggml.tokens");
    if(!tokens || tokens->type!=9 || tokens->element_type!=8 || tokens->count!=c->vocab)return -1;
    const LmbGgufValue *ev=lmb_q38g_value(g,"ple.eos_token_id");
    if(!ev)ev=lmb_gguf_get(g,"tokenizer.ggml.eos_token_id");
    if(lmb_gguf_integer(ev,&eos) || eos>=c->vocab)return -1;
    c->eos=(uint32_t)eos;
    const LmbGgufValue *ratios=lmb_q38g_value(g,"attention.compress_ratios"),
        *rec=lmb_q38g_value(g,"attention.recurrent_layers"),*ple=lmb_q38g_value(g,"ple.layers");
    uint32_t every=0;uint64_t pl;
    if(!ratios || ratios->type!=9 || ratios->count!=c->layers ||
       (rec && (rec->type!=9 || rec->count!=c->layers)) ||
       (!rec && lmb_q38g_uint(g,"full_attention_interval",0,1,c->layers,&every)) ||
       !ple || ple->count!=1 || lmb_gguf_inventory_item(ple,0,&pl) || pl>=c->layers)return -1;
    c->ple=(uint32_t)pl;
    for(uint32_t l=0;l<c->layers;l++) {
        uint64_t r,recurrent=every?(l+1)%every!=0:0;
        if(lmb_gguf_inventory_item(ratios,l,&r) || r>262144 ||
           (rec && lmb_gguf_inventory_item(rec,l,&recurrent)) || recurrent>1 || recurrent!=(r==0))return -1;
        c->full[l]=(uint8_t)!recurrent;
        if(r){if(c->ratio && c->ratio!=r)return -1;c->ratio=(uint32_t)r;}
    }
    if(!c->ratio || c->budget%c->ratio || lmb_gguf_number(lmb_q38g_value(g,"rope.freq_base"),1e7,&c->theta) ||
       lmb_gguf_number(lmb_q38g_value(g,"attention.layer_norm_rms_epsilon"),1e-6,&c->eps) ||
       c->theta<=0 || c->theta>FLT_MAX || c->eps<=0 || c->eps>FLT_MAX)return -1;
    return 0;
}
static int lmb_q38g_matches(const char *cfg,const LmbQ38Gguf *c) {
    const char *keys[]={"num_hidden_layers","hidden_size","vocab_size","max_position_embeddings","eos_token_id",
        "hc_count","hc_lowrank","num_attention_heads","num_key_value_heads","head_dim","indexer_n_heads",
        "indexer_kv_heads","indexer_head_dim","indexer_budget","indexer_compress_ratio","num_experts",
        "num_experts_per_tok","moe_intermediate_size","shared_expert_intermediate_size","linear_num_key_heads",
        "linear_num_value_heads","linear_key_head_dim","linear_value_head_dim","linear_conv_kernel_dim",
        "ngram_size","heads_per_ngram","ple_conv_kernel_size","ple_embed_dim","split_ngram_parts"};
    uint32_t values[]={c->layers,c->h,c->vocab,c->context,c->eos,c->hc,c->rank,c->qh,c->kv,c->hd,c->ih,1,c->id,c->budget,
        c->ratio,c->experts,c->topk,c->inter,c->shared,c->kh,c->vh,c->kd,c->vd,c->conv,3,c->nh/2,c->pc,c->pd,1};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++){uint32_t v;if(lmb_plan_u32(cfg,keys[i],values[i],values[i],&v))return -1;}
    uint8_t full[LMB_PLAN_LAYER_MAX];uint64_t ple[1];unsigned count;double eps,theta,partial;
    const char *rope=lmb_json_member(cfg,"rope_parameters");char str[48];
    if(lmb_plan_kinds(cfg,"layer_types","linear_attention","full_attention",full,c->layers) ||
       memcmp(full,c->full,c->layers) || lmb_plan_array(lmb_json_member(cfg,"ple_layer_ids"),ple,1,&count) ||
       count!=1 || ple[0]!=c->ple+1u || !rope || *rope!='{' ||
       lmb_json_string(rope,"rope_type",str,sizeof str) || strcmp(str,"default") ||
       lmb_plan_number(rope,"rope_theta",NAN,&theta) || (float)theta!=(float)c->theta ||
       lmb_plan_number(rope,"partial_rotary_factor",NAN,&partial) || fabs(partial-(double)c->rot/c->hd)>1e-7 ||
       lmb_plan_number(cfg,"rms_norm_eps",NAN,&eps) || (float)eps!=(float)c->eps)return -1;
    const char *truth=lmb_json_member(cfg,"norm_topk_prob"),*bias=lmb_json_member(cfg,"attention_bias"),
        *tie=lmb_json_member(cfg,"tie_word_embeddings");
    if(!truth || strncmp(truth,"true",4) || (bias && strncmp(bias,"false",5)) || (tie && strncmp(tie,"false",5)) ||
       lmb_json_string(cfg,"output_gate_type",str,sizeof str) || strcmp(str,"sigmoid") ||
       lmb_json_string(cfg,"hidden_act",str,sizeof str) || strcmp(str,"silu"))return -1;
    return 0;
}
static int lmb_q38g_memory(const LmbGguf *g,const char *cfg,LmbModelShape *m) {
    LmbQ38Gguf c;if(lmb_q38g_geometry(g,&c) || lmb_q38g_matches(cfg,&c) ||
        m->layers!=c.layers || m->hidden!=c.h || m->vocab!=c.vocab)return -1;
    LmbGgufInventory v={.g=g,.seen=calloc(g->ntensors,1)};if(!v.seen)return -1;
    uint64_t H=c.h,HC=H*c.hc,R=c.rank,Q=(uint64_t)c.qh*c.hd,K=(uint64_t)c.kv*c.hd,DV=(uint64_t)c.vh*c.vd;
    char name[128];int rc=-1;
#define QW(n,r,x,y,z,mat,cost) if(lmb_gguf_inventory_weight(&v,n,r,x,y,z,mat,cost))goto done
    QW("token_embd.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    QW("output.weight",2,H,c.vocab,1,1,&m->edge_resident_bytes);
    QW("output_hc_norm.weight",1,HC,1,1,0,&m->edge_resident_bytes);
    QW("output_hc_down.weight",2,HC,R,1,1,&m->edge_resident_bytes);
    QW("output_hc_up.weight",2,R,HC,1,1,&m->edge_resident_bytes);
    for(uint32_t l=0;l<c.layers;l++) {
        uint64_t *cost=&m->memory[l].resident_bytes;
#define LW(s,r,x,y,z,mat) do{snprintf(name,sizeof name,"blk.%u.%s",l,s);QW(name,r,x,y,z,mat,cost);}while(0)
        const char *prefix[]={"hc_attn","hc_ffn"};
        for(unsigned i=0;i<2;i++) {
            snprintf(name,sizeof name,"blk.%u.%s_norm.weight",l,prefix[i]);QW(name,1,HC,1,1,0,cost);
            snprintf(name,sizeof name,"blk.%u.%s_down.weight",l,prefix[i]);QW(name,2,HC,R,1,1,cost);
            snprintf(name,sizeof name,"blk.%u.%s_up.weight",l,prefix[i]);QW(name,2,R,HC,1,1,cost);
            snprintf(name,sizeof name,"blk.%u.%s_inject.weight",l,prefix[i]);QW(name,2,HC,c.hc,1,0,cost);
        }
        LW("ffn_gate_inp.weight",2,H,c.experts,1,0);LW("ffn_gate_inp_shexp.weight",1,H,1,1,0);
        LW("ffn_gate_shexp.weight",2,H,c.shared,1,1);LW("ffn_up_shexp.weight",2,H,c.shared,1,1);
        LW("ffn_down_shexp.weight",2,c.shared,H,1,1);
        LW("ffn_gate_exps.weight",3,H,c.inter,c.experts,1);LW("ffn_up_exps.weight",3,H,c.inter,c.experts,1);
        LW("ffn_down_exps.weight",3,c.inter,H,c.experts,1);
        if(c.full[l]) {
            LW("attn_q.weight",2,H,2*Q,1,1);LW("attn_k.weight",2,H,K,1,1);LW("attn_v.weight",2,H,K,1,1);
            LW("attn_output.weight",2,Q,H,1,1);LW("attn_q_norm.weight",1,c.hd,1,1,0);LW("attn_k_norm.weight",1,c.hd,1,1,0);
            LW("indexer.q_proj.weight",2,H,(uint64_t)c.ih*c.id,1,1);LW("indexer.k_proj.weight",2,H,c.id,1,1);
            snprintf(name,sizeof name,"blk.%u.indexer.q_proj.weight",l);const LmbGgufTensor *qt=lmb_gguf_tensor(g,name);
            snprintf(name,sizeof name,"blk.%u.indexer.k_proj.weight",l);const LmbGgufTensor *kt=lmb_gguf_tensor(g,name);
            if(qt->type!=kt->type)goto done;
            LW("indexer.q_norm.weight",1,c.id,1,1,0);LW("indexer.k_norm.weight",1,c.id,1,1,0);
            m->memory[l].state_token_bytes=(2*K+c.id)*4;
        } else {
            LW("attn_qkv.weight",2,H,c.cd,1,1);LW("attn_gate.weight",2,H,DV,1,1);
            LW("ssm_alpha.weight",2,H,c.vh,1,0);LW("ssm_beta.weight",2,H,c.vh,1,0);
            LW("ssm_conv1d.weight",2,c.conv,c.cd,1,0);LW("ssm_dt.bias",1,c.vh,1,1,0);
            LW("ssm_a",1,c.vh,1,1,0);LW("ssm_norm.weight",1,c.vd,1,1,0);LW("ssm_out.weight",2,DV,H,1,1);
            m->memory[l].state_fixed_bytes=((uint64_t)c.vh*c.kd*c.vd+(uint64_t)c.cd*(c.conv-1u))*4;
        }
        if(l==c.ple) {
            LW("ple_key.weight",2,c.pd,HC,1,1);LW("ple_value.weight",2,c.pd,H,1,1);
            LW("ple_norm_key.weight",1,HC,1,1,0);LW("ple_norm_query.weight",1,HC,1,1,0);
            LW("ple_norm_conv.weight",1,HC,1,1,0);LW("ple_conv1d.weight",2,c.pc,HC,1,0);
            m->memory[l].state_fixed_bytes=lmb_size_add(m->memory[l].state_fixed_bytes,HC*(c.pc-1u)*3u*4u+32u);
        }
#undef LW
    }
    const LmbGgufTensor *table=lmb_gguf_tensor(g,"per_layer_token_embd.weight");
    if(!table || table->rank!=2 || table->shape[0]!=c.nd || !table->shape[1] ||
       !LMB_GGUF_TYPES[table->type].decodable || table->bytes/table->shape[1]>1024)goto done;
    const char *hashkeys[]={"ple.layer_multipliers","ple.head_vocab_sizes","ple.head_offsets"};
    for(unsigned i=0;i<3;i++) {
        const LmbGgufValue *a=lmb_q38g_value(g,hashkeys[i]);
        if(!a || a->type!=9 || (a->element_type!=10 && a->element_type!=11) || a->count!=(i?c.nh:3))goto done;
    }
    for(uint32_t i=0;i<c.nh;i++) {
        uint64_t size,off;
        if(lmb_gguf_inventory_item(lmb_q38g_value(g,hashkeys[1]),i,&size) || !size ||
           lmb_gguf_inventory_item(lmb_q38g_value(g,hashkeys[2]),i,&off) || off>INT64_MAX || size>INT64_MAX-off ||
           off+size>table->shape[1])goto done;
    }
    v.seen[table-g->tensors]=1;
    m->memory[c.ple].resident_bytes=lmb_size_add(m->memory[c.ple].resident_bytes,table->bytes);
    for(uint32_t i=0;i<g->ntensors;i++)if(!v.seen[i])goto done;
    m->boundary_width=(uint32_t)HC;
    uint64_t index=lmb_size_add(lmb_size_mul(g->header_bytes,6),lmb_size_mul(g->ntensors,1024));
    m->segment_fixed_bytes=lmb_size_add(index,(uint64_t)c.layers*(16384u+(uint64_t)c.experts*1024u));
    uint64_t wide=HC+R+H+c.experts+c.topk+c.inter+c.shared+c.cd+Q+(uint64_t)c.ih*c.id+c.pd+c.budget;
    m->scratch_fixed_bytes=lmb_size_add(lmb_size_add(v.largest,UINT64_C(64)<<20),lmb_size_mul(wide,128u*64u));
    m->scratch_token_bytes=256u*16u;m->edge_scratch_fixed_bytes=v.largest;
    m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,index);
    m->experts=c.experts;m->experts_per_tok=c.topk;m->moe_intermediate=c.inter;
    m->max_context=c.context;m->memory_contract=1;rc=0;
done:
#undef QW
    free(v.seen);return rc;
}
#endif
