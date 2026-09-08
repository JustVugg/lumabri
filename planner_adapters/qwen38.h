#ifndef LUMABRI_PLAN_QWEN38_H
#define LUMABRI_PLAN_QWEN38_H
/* Text-only float/BF16 Qwen4-Exp contract, pinned Colibri 12a5c464.
 * PLE tables are row-read by the native engine: reserve their complete source
 * footprint as well as all expert slots, without advertising a disk mode or
 * claiming that admission has warmed the filesystem cache. */
#include <float.h>
#include <math.h>
typedef struct {
    LmbModelShape *m;
    uint32_t h, hc, rank, qh, kvh, hd, ih, id, budget, ratio;
    uint32_t e, k, inter, shared, kh, vh, kd, vd, conv, cd;
    uint32_t pd, pc, nh, nd, parts, ple;
    uint8_t full[LMB_PLAN_LAYER_MAX], kinds[LMB_PLAN_LAYER_MAX], prefix, edge;
    uint64_t seen[LMB_PLAN_LAYER_MAX], largest, rows[512], vocab[64], offsets[64];
    uint8_t *experts;
    uint32_t ple_seen;
    int table_whole;
} LmbQwen38Inventory;

static int lmb_q38_float(const char *s) {
    return !strcmp(s,"BF16") ? 1 : !strcmp(s,"F16") ? 2 : !strcmp(s,"F32") ? 4 : 0;
}
static int lmb_q38_shape(const LmbPlanTensor *t, uint64_t n, uint64_t cols) {
    return t->elements==n && (!cols || (t->rank==2 && t->shape[1]==cols));
}
static int lmb_q38_tensor(const LmbPlanTensor *t, void *opaque) {
    LmbQwen38Inventory *v=opaque;
    LmbModelShape *m=v->m;
    const char *name=t->name;
    if (strcmp(name,"lm_head.weight")) {
        uint8_t prefix=!strncmp(name,"model.language_model.",21) ? 2 : !strncmp(name,"model.",6) ? 1 : 0;
        if (!prefix) return 0;
        if (v->prefix && v->prefix!=prefix) return -1;
        v->prefix=prefix; name+=prefix==2 ? 21 : 6;
    }
    uint64_t H=v->h, HC=H*v->hc, R=v->rank;
    static const char *const globals[]={"embed_tokens.weight","lm_head.weight",
        "hyper_connection_mixer.hc_norm.weight",
        "hyper_connection_mixer.input_mix_weight_down.weight",
        "hyper_connection_mixer.input_mix_weight_up.weight"};
    for (unsigned i=0;i<5;i++) if (!strcmp(name,globals[i])) {
        uint64_t n=i<2 ? H*m->vocab : i==2 ? HC : HC*R;
        uint64_t cols=i<2 ? H : i==2 ? 0 : i==3 ? HC : R;
        if (!lmb_q38_float(t->dtype) || (v->edge&(1u<<i)) || !lmb_q38_shape(t,n,cols)) return -1;
        v->edge|=1u<<i;
        m->edge_resident_bytes=lmb_size_add(m->edge_resident_bytes,n*4);
        if (n*4>m->edge_scratch_fixed_bytes) m->edge_scratch_fixed_bytes=n*4;
        return m->edge_resident_bytes==UINT64_MAX ? -1 : 0;
    }
    if (strncmp(name,"layers.",7)) return 0;
    uint64_t layer;
    const char *field=lmb_plan_uint(name+7,&layer);
    if (!field || *field++!='.' || layer>=m->layers) return -1;
    uint64_t bytes=0;
    if (!strncmp(field,"ple.",4)) {
        if (layer!=v->ple) return -1;
        static const char *const ple_names[]={"ple.key_proj.weight","ple.value_proj.weight",
            "ple.norm_key.weight","ple.norm_query.weight","ple.norm_conv.weight","ple.conv1d.weight",
            "ple.ple_embedding.layer_multipliers","ple.ple_embedding.ngram_heads_vocab_sizes",
            "ple.ple_embedding.ngram_heads_offsets","ple.ple_embedding.ngram_embedding.weight_scale"};
        unsigned i;
        for(i=0;i<10;i++) if(!strcmp(field,ple_names[i])) break;
        if (i<10) {
            uint64_t counts[]={HC*v->pd,H*v->pd,HC,HC,HC,HC*v->pc,3,v->nh,v->nh,1};
            if ((v->ple_seen&(1u<<i)) || !lmb_q38_shape(t,counts[i],i<2 ? v->pd : 0)) return -1;
            v->ple_seen|=1u<<i;
            if (i>=6 && i<=8) {
                if (strcmp(t->dtype,"I64")) return -1;
                if (i==7) memcpy(v->vocab,t->meta_i64,v->nh*sizeof(uint64_t));
                if (i==8) memcpy(v->offsets,t->meta_i64,v->nh*sizeof(uint64_t));
                bytes=t->bytes;
            } else {
                if (!lmb_q38_float(t->dtype)) return -1;
                bytes=t->elements*4;
            }
        } else {
            unsigned part=0;
            if (!strcmp(field,"ple.ple_embedding.ngram_embedding.weight")) {
                if (v->table_whole || v->rows[0]) return -1;
                v->table_whole=1;
            } else {
                const char *prefix="ple.ple_embedding.ngram_embedding.shard_";
                uint64_t index;
                if (strncmp(field,prefix,strlen(prefix))) return -1;
                const char *end=lmb_plan_uint(field+strlen(prefix),&index);
                if (!end || strcmp(end,".weight") || index>=v->parts || v->table_whole) return -1;
                part=(unsigned)index;
            }
            if (v->rows[part] || !lmb_q38_float(t->dtype) || t->rank!=2 ||
                !t->shape[0] || t->shape[1]!=v->nd) return -1;
            v->rows[part]=t->shape[0];
            bytes=t->bytes; /* full PLE source cache allowance, not allocation */
        }
    } else if (!strncmp(field,"mlp.experts.",12)) {
        const char *suffix=field+12;
        unsigned kind=lmb_q38_float(t->dtype);
        if (!kind) return -1;
        uint64_t count=H*v->inter;
        if (!strcmp(suffix,"gate_up_proj") || !strcmp(suffix,"down_proj")) {
            unsigned up=!strcmp(suffix,"gate_up_proj");
            unsigned bit=up ? 1 : 2;
            if (t->rank!=3 || t->shape[0]!=v->e || t->shape[1]!=(up ? 2u*v->inter : H) ||
                t->shape[2]!=(up ? H : v->inter)) return -1;
            for (unsigned e=0;e<v->e;e++) {
                uint8_t *seen=&v->experts[layer*v->e+e];
                if ((*seen&7) || (*seen&(bit<<3))) return -1;
                *seen|=bit<<3;
            }
            bytes=lmb_size_mul(lmb_size_mul(count,up ? 2 : 1),4u*v->e);
        } else {
            uint64_t expert;
            const char *end=lmb_plan_uint(suffix,&expert);
            if (!end || *end++!='.' || expert>=v->e) return -1;
            unsigned k=!strcmp(end,"gate_proj.weight") ? 0 : !strcmp(end,"up_proj.weight") ? 1 :
                       !strcmp(end,"down_proj.weight") ? 2 : 3;
            uint8_t *seen=&v->experts[layer*v->e+expert];
            if (k==3 || (*seen&24) || (*seen&(1u<<k)) ||
                !lmb_q38_shape(t,count,k==2 ? v->inter : H)) return -1;
            *seen|=1u<<k; bytes=count*4;
        }
        v->kinds[layer]|=kind;
    } else {
        static const char *const names[]={
            "attn_hyper_connection.hc_norm.weight","attn_hyper_connection.input_mix_weight_down.weight",
            "attn_hyper_connection.input_mix_weight_up.weight","attn_hyper_connection.block_inject_weight.weight",
            "mlp_hyper_connection.hc_norm.weight","mlp_hyper_connection.input_mix_weight_down.weight",
            "mlp_hyper_connection.input_mix_weight_up.weight","mlp_hyper_connection.block_inject_weight.weight",
            "mlp.gate.weight","mlp.shared_expert.gate_proj.weight","mlp.shared_expert.up_proj.weight",
            "mlp.shared_expert.down_proj.weight","mlp.shared_expert_gate.weight",
            "self_attn.q_proj.weight","self_attn.k_proj.weight","self_attn.v_proj.weight","self_attn.o_proj.weight",
            "self_attn.q_norm.weight","self_attn.k_norm.weight","self_attn.indexer.index_qk_proj.weight",
            "self_attn.indexer.q_layernorm.weight","self_attn.indexer.k_layernorm.weight",
            "linear_attn.in_proj_qkv.weight","linear_attn.in_proj_z.weight","linear_attn.in_proj_b.weight",
            "linear_attn.in_proj_a.weight","linear_attn.conv1d.weight","linear_attn.dt_bias","linear_attn.A_log",
            "linear_attn.norm.weight","linear_attn.out_proj.weight"};
        uint64_t Q=(uint64_t)v->qh*v->hd, KV=(uint64_t)v->kvh*v->hd, DV=(uint64_t)v->vh*v->vd;
        uint64_t counts[]={HC,HC*R,HC*R,HC*v->hc,HC,HC*R,HC*R,HC*v->hc,
            H*v->e,H*v->shared,H*v->shared,H*v->shared,H,
            2*Q*H,KV*H,KV*H,H*Q,v->hd,v->hd,(v->ih+1u)*(uint64_t)v->id*H,v->id,v->id,
            (uint64_t)v->cd*H,DV*H,(uint64_t)v->vh*H,(uint64_t)v->vh*H,
            (uint64_t)v->cd*v->conv,v->vh,v->vh,v->vd,H*DV};
        uint64_t cols[]={0,HC,R,HC,0,HC,R,HC,H,H,H,v->shared,0,
            H,H,H,Q,0,0,H,0,0,H,H,H,H,0,0,0,0,DV};
        unsigned i;
        for(i=0;i<31;i++) if(!strcmp(field,names[i])) break;
        if (i==31) return 0;
        if (!lmb_q38_float(t->dtype) || (v->seen[layer]&(UINT64_C(1)<<i)) ||
            !lmb_q38_shape(t,counts[i],cols[i])) return -1;
        v->seen[layer]|=UINT64_C(1)<<i;
        bytes=lmb_size_mul(t->elements,4);
    }
    m->memory[layer].resident_bytes=lmb_size_add(m->memory[layer].resident_bytes,bytes);
    if (bytes>v->largest) v->largest=bytes;
    return bytes==UINT64_MAX || m->memory[layer].resident_bytes==UINT64_MAX ? -1 : 0;
}

static int LMB_UNUSED lmb_qwen38_memory(const char *root, const char *whole,
                                        const char *json, LmbModelShape *m) {
    if (lmb_json_member(whole,"vision_config")) return -1;
    LmbQwen38Inventory v={.m=m};
    uint32_t layers, maxctx, eos, ik, ns;
    char str[64];
    if (lmb_json_string(json,"output_gate_type",str,sizeof str) || strcmp(str,"sigmoid")) return -1;
    if (lmb_json_member(json,"hidden_act") &&
        (lmb_json_string(json,"hidden_act",str,sizeof str) || strcmp(str,"silu"))) return -1;
    const char *flags[]={"attention_bias","tie_word_embeddings"};
    for(unsigned i=0;i<2;i++) {
        const char *p=lmb_json_member(json,flags[i]);
        if(p && (strncmp(p,"false",5) || (*lmb_plan_space(p+5)!=',' && *lmb_plan_space(p+5)!='}'))) return -1;
    }
    const char *norm_topk=lmb_json_member(json,"norm_topk_prob");
    if(norm_topk) {
        unsigned n=!strncmp(norm_topk,"true",4) ? 4 : !strncmp(norm_topk,"false",5) ? 5 : 0;
        if(!n || (*lmb_plan_space(norm_topk+n)!=',' && *lmb_plan_space(norm_topk+n)!='}')) return -1;
    }
#define NEED(key,dst,lo,hi) if(lmb_plan_u32(json,key,lo,hi,&dst)) return -1
    NEED("hidden_size",v.h,1,65536); NEED("num_hidden_layers",layers,1,LMB_PLAN_LAYER_MAX);
    NEED("max_position_embeddings",maxctx,1,262144); NEED("eos_token_id",eos,0,INT32_MAX);
    NEED("num_attention_heads",v.qh,1,65536); NEED("num_key_value_heads",v.kvh,1,65536);
    NEED("head_dim",v.hd,1,65536); NEED("indexer_n_heads",v.ih,1,65536);
    NEED("indexer_kv_heads",ik,1,1); NEED("indexer_head_dim",v.id,1,65536);
    NEED("indexer_budget",v.budget,1,262144); NEED("indexer_compress_ratio",v.ratio,1,262144);
    NEED("num_experts",v.e,1,1024); NEED("num_experts_per_tok",v.k,1,256);
    NEED("moe_intermediate_size",v.inter,1,1048576); NEED("shared_expert_intermediate_size",v.shared,1,1048576);
    NEED("linear_num_key_heads",v.kh,1,65536); NEED("linear_num_value_heads",v.vh,1,65536);
    NEED("linear_key_head_dim",v.kd,1,65536); NEED("linear_value_head_dim",v.vd,1,512);
    NEED("linear_conv_kernel_dim",v.conv,2,65536);
#undef NEED
    if(v.h!=m->hidden || layers!=m->layers || !m->vocab || m->vocab>(1u<<22) || eos>=m->vocab ||
        v.k>v.e || v.qh%v.kvh || v.vh%v.kh || v.budget%v.ratio ||
        lmb_plan_optional(json,"hc_count",4,2,16,&v.hc) ||
        lmb_plan_optional(json,"hc_lowrank",320,1,65536,&v.rank) ||
        lmb_plan_optional(json,"ple_embed_dim",v.h,1,65536,&v.pd) ||
        lmb_plan_optional(json,"ple_conv_kernel_size",4,2,65536,&v.pc) ||
        lmb_plan_optional(json,"ngram_size",3,3,3,&ns) ||
        lmb_plan_optional(json,"heads_per_ngram",8,1,32,&v.nh) ||
        lmb_plan_optional(json,"split_ngram_parts",1,1,512,&v.parts)) return -1;
    v.nh*=2; v.nd=v.pd/v.nh;
    uint64_t cd=2u*(uint64_t)v.kh*v.kd+(uint64_t)v.vh*v.vd;
    if(!v.nd || v.nd>512 || v.pd%v.nh || cd>INT32_MAX ||
        2u*(uint64_t)v.qh*v.hd>INT32_MAX || (v.ih+1u)*(uint64_t)v.id>INT32_MAX) return -1;
    v.cd=(uint32_t)cd;
    const char *rp=lmb_json_member(json,"rope_parameters");
    if(rp && *rp!='{') return -1;
    if(rp && lmb_json_member(rp,"rope_type") &&
       (lmb_json_string(rp,"rope_type",str,sizeof str) || strcmp(str,"default"))) return -1;
    double partial, theta, eps;
    if(lmb_plan_number(json,"partial_rotary_factor",1,&partial) ||
       lmb_plan_number(rp ? rp : json,"partial_rotary_factor",partial,&partial) ||
       lmb_plan_number(rp ? rp : json,"rope_theta",10000,&theta) ||
       lmb_plan_number(json,"rms_norm_eps",1e-6,&eps) ||
       theta>FLT_MAX || eps>FLT_MAX || (float)theta<=0 || (float)eps<=0 || partial<=0 || partial>1) return -1;
    unsigned rotary=(unsigned)(v.hd*partial);
    if(!rotary || (rotary&1) || v.id<rotary) return -1;
    uint64_t ple_id[1]; unsigned count;
    if(lmb_plan_array(lmb_json_member(json,"ple_layer_ids"),ple_id,1,&count) || count!=1 ||
       !ple_id[0] || ple_id[0]>layers) return -1;
    v.ple=(uint32_t)ple_id[0]-1;
    const char *types=lmb_json_member(json,"layer_types");
    if(!types || *types++!='[') return -1;
    for(unsigned i=0;i<layers;i++) {
        types=lmb_plan_space(types);
        if(!(types=lmb_plan_string(types,str,sizeof str))) return -1;
        if(!strcmp(str,"linear_attention")) v.full[i]=0;
        else if(!strcmp(str,"full_attention") || !strcmp(str,"qwen_sparse_attention")) v.full[i]=1;
        else return -1;
        types=lmb_plan_space(types);
        if(*types++!=(i+1==layers ? ']' : ',')) return -1;
    }
    v.experts=calloc((size_t)layers*v.e,1);
    if(!v.experts) return -1;
    int rc=lmb_plan_tensors(root,lmb_q38_tensor,&v);
    if(v.edge!=31 || (v.ple_seen&511)!=511) rc=-1;
    uint64_t rows=0;
    for(unsigned i=0;i<(v.table_whole ? 1 : v.parts);i++) {
        if(!v.rows[i]) rc=-1;
        rows=lmb_size_add(rows,v.rows[i]);
    }
    if(v.table_whole) for(unsigned i=1;i<512;i++) if(v.rows[i]) rc=-1;
    for(unsigned i=0;i<v.nh;i++) if(!v.vocab[i] || v.offsets[i]>INT64_MAX ||
        v.vocab[i]>(uint64_t)INT64_MAX-v.offsets[i] || v.offsets[i]+v.vocab[i]>rows) rc=-1;
    for(unsigned i=0;i<layers && !rc;i++) {
        uint64_t need=8191u | (v.full[i] ? ((UINT64_C(1)<<22)-(UINT64_C(1)<<13)) :
                                            ((UINT64_C(1)<<31)-(UINT64_C(1)<<22)));
        if((v.seen[i]&need)!=need || v.kinds[i]!=v.kinds[0]) {rc=-1;break;}
        uint8_t layout=v.experts[i*v.e];
        for(unsigned e=0;e<v.e;e++) if((layout!=7 && layout!=24) || v.experts[i*v.e+e]!=layout) rc=-1;
        if(v.full[i]) m->memory[i].state_token_bytes=(2u*(uint64_t)v.kvh*v.hd+v.id)*4;
        else m->memory[i].state_fixed_bytes=((uint64_t)v.vh*v.kd*v.vd+cd*(v.conv-1u))*4;
    }
    free(v.experts);
    if(rc || rows>INT64_MAX) return -1;
    m->boundary_width=v.h*v.hc;
    m->memory[v.ple].state_fixed_bytes=lmb_size_add(m->memory[v.ple].state_fixed_bytes,
        (uint64_t)m->boundary_width*(v.pc-1u)*3*4+32);
    uint64_t wide=(uint64_t)m->boundary_width+v.rank+v.h+v.e+v.k+v.inter+v.shared+
        cd+(uint64_t)v.qh*v.hd+(uint64_t)v.ih*v.id+v.pd+v.budget;
    /* 128 input rows, 32-row grouped-MoE workspace capped at 64 MiB;
     * attention temporaries include per-thread context scores. */
    m->scratch_fixed_bytes=lmb_size_add(lmb_size_add(v.largest,UINT64_C(64)<<20),lmb_size_mul(wide,128u*64));
    m->scratch_token_bytes=256u*16;
    m->segment_fixed_bytes=(uint64_t)layers*(16384u+(uint64_t)v.e*1024u);
    m->max_context=maxctx; m->memory_contract=1;
    return 0;
}
#endif
