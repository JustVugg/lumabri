/* Public Colibri ABI laboratory. Not registered by the product.
 * Bounded-budget opens are deliberately REFUSED until the checkpoint inspector
 * accounts for Engram + foreign KV + scratch. Never silently ignore a budget.
 * The pinned CLI loader still has fatal-error paths: this is not a safe public
 * checkpoint loader yet. The ordinary Lumabri binaries do not link this header.
 */
#ifndef LMB_V41_ABI_LAB_H
#define LMB_V41_ABI_LAB_H
#include "segment_adapter_internal.h"
#include "edge_tok_internal.h"
#include "v41_boundary.h"
#include "v41_lifecycle.h"

#define LMB_V41_SCHEMA "lmb-v41-text-delta1-lab-v1"
#define LMB_V41_NUMERIC "v41/f32/single-row-cpu-v1"
static pthread_mutex_t lmb_v41_runtime_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct {Model weights; unsigned sessions;} LmbV41Engine;
typedef struct {LmbV41Engine *engine;Model state;int counts[V41_MAX_LAYERS];uint32_t context;} LmbV41Session;
typedef struct {Model weights;Tok tokenizer;size_t width;} LmbV41Edge;

static size_t lmb_v41_state_width(const Cfg *c) {
    return 2+(size_t)c->hc_mult*(c->dim+1)+lmb_v41_aux_width(c);
}
static size_t lmb_v41_state_aux(const Cfg *c) {return 2+(size_t)c->hc_mult*(c->dim+1);}

static int lmb_v41_layout_valid(const Cfg *c) {
    if(c->n_layers<1 || c->n_layers>V41_MAX_LAYERS || c->dim<1 || c->dim>1048576 ||
       c->hc_mult<1 || c->hc_mult>16 || c->vocab<1 || c->vocab>16777216 ||
       c->max_positions<2 || c->max_positions>1048576 || c->head_dim<1 || c->head_dim>65536 ||
       c->index_head_dim<1 || c->index_head_dim>65536 || c->index_topk<1 || c->index_topk>1048576 ||
       c->candidate_topk_blocks<0 || c->candidate_topk_blocks>1048576 ||
       lmb_v41_state_width(c)>1048576) return 0;
    for(int i=0;i<c->n_layers;i++) if(c->kv_source[i] &&
        (c->compress_ratio[i]<1 || c->compress_ratio[i]>c->max_positions)) return 0;
    return 1;
}
static int lmb_v41_row_valid(const Cfg *c,const float *in,size_t bytes) {
    size_t n=lmb_v41_state_width(c),aux=lmb_v41_state_aux(c);
    if(!in || bytes!=n*sizeof(float) || in[0]!=(float)aux || in[1]!=(float)(n-aux)) return 0;
    for(size_t i=2;i<aux;i++) if(!isfinite(in[i])) return 0;
    return 1;
}
static int lmb_v41_segment_open(void **impl,ColiSegmentCapabilities *cap,
    const ColiSegmentEngineOptions *o,char *error,size_t error_size) {
    *impl=NULL;
    if(o->memory_limit_bytes || o->resource_plan_size)
        return coli_segment_adapter_error(error,error_size,"V4.1 bounded admission is not implemented; laboratory only");
    if(o->backend_mask && o->backend_mask!=COLI_SEGMENT_CAP_CPU)
        return coli_segment_adapter_error(error,error_size,"V4.1 laboratory is CPU only");
    Cfg c;cfg_load(&c,o->model_dir);
    if(!lmb_v41_layout_valid(&c) || o->layer_begin>=o->layer_end || o->layer_end>(uint32_t)c.n_layers ||
       o->context_tokens<2 || o->context_tokens>(uint32_t)c.max_positions)
        return coli_segment_adapter_error(error,error_size,"invalid V4.1 range or context");
    LmbV41Engine *e=calloc(1,sizeof *e);if(!e)return -1;
    e->weights.range_enabled=1;e->weights.range_begin=(int)o->layer_begin;e->weights.range_end=(int)o->layer_end;
    e->weights.context_limit=(int)o->context_tokens;
    pthread_mutex_lock(&lmb_v41_runtime_lock);
    model_load(&e->weights,o->model_dir,c.n_routed,32);
    for(uint32_t l=o->layer_begin;l<o->layer_end;l++)
        for(int k=0;k<c.n_routed;k++)(void)expert_slot_at(&e->weights,&e->weights.cache[l],"layers",(int)l,k);
    pthread_mutex_unlock(&lmb_v41_runtime_lock);
    memset(cap,0,sizeof *cap);cap->struct_size=sizeof *cap;cap->abi_version=COLI_SEGMENT_ABI_VERSION;
    cap->flags=COLI_SEGMENT_CAP_TOKEN_IDS|COLI_SEGMENT_CAP_RANGE_NATIVE|COLI_SEGMENT_CAP_MULTI_SESSION|COLI_SEGMENT_CAP_CPU;
    snprintf(cap->engine_id,sizeof cap->engine_id,"deepseek_v41");
    snprintf(cap->state_schema,sizeof cap->state_schema,LMB_V41_SCHEMA);
    snprintf(cap->numeric_class,sizeof cap->numeric_class,LMB_V41_NUMERIC);
    cap->state_dtype=COLI_SEGMENT_DTYPE_F32;cap->state_width=(uint32_t)lmb_v41_state_width(&c);
    cap->max_batch_rows=1;cap->max_context_tokens=o->context_tokens;cap->num_layers=(uint32_t)c.n_layers;
    *impl=e;return 0;
}
static void lmb_v41_segment_destroy(void *impl) {
    LmbV41Engine *e=impl;if(!e)return;lmb_v41_model_destroy(&e->weights,1);free(e);
}
static int lmb_v41_session_create(void *impl,void **out,const ColiSegmentSessionOptions *o,char *err,size_t size) {
    LmbV41Engine *e=impl;*out=NULL;
    if(o->memory_limit_bytes || o->context_tokens<2 || o->context_tokens>(uint32_t)e->weights.c.max_positions)
        return coli_segment_adapter_error(err,size,"invalid or bounded V4.1 laboratory session");
    LmbV41Session *s=calloc(1,sizeof *s);if(!s)return -1;
    pthread_mutex_lock(&lmb_v41_runtime_lock);
    int rc=lmb_v41_session_model(&s->state,&e->weights);
    if(!rc)e->sessions++;
    pthread_mutex_unlock(&lmb_v41_runtime_lock);
    if(rc){free(s);return -1;}s->engine=e;s->context=o->context_tokens;*out=s;return 0;
}
static void lmb_v41_session_destroy(void *impl) {
    LmbV41Session *s=impl;if(!s)return;
    pthread_mutex_lock(&lmb_v41_runtime_lock);lmb_v41_model_destroy(&s->state,0);s->engine->sessions--;
    pthread_mutex_unlock(&lmb_v41_runtime_lock);free(s);
}
static int lmb_v41_session_run(void *impl,const ColiSegmentRunRequest *r,char *err,size_t size) {
    LmbV41Session *s=impl;Model *m=&s->state;const Cfg *c=&m->c;
    size_t aux=lmb_v41_state_aux(c),width=lmb_v41_state_width(c);
    if(r->rows!=1 || r->token_count!=1 || !r->token_ids || r->token_ids[0]<0 || r->token_ids[0]>=c->vocab ||
       r->position>=s->context || !r->output || r->output_bytes!=width*4 ||
       !lmb_v41_row_valid(c,r->input,r->input_bytes))
        return coli_segment_adapter_error(err,size,"invalid V4.1 single-row input");
    pthread_mutex_lock(&lmb_v41_runtime_lock);
    const float *in=r->input;float *out=r->output;
    if(r->position!=(uint64_t)m->pos || (r->should_cancel && r->should_cancel(r->cancel_user_data)) ||
       lmb_v41_aux_read(m,s->counts,in+aux,width-aux)) {
        pthread_mutex_unlock(&lmb_v41_runtime_lock);
        return coli_segment_adapter_error(err,size,"V4.1 cancelled, out-of-order or invalid shared state");
    }
    m->boundary_input=in+2;m->boundary_mix=in+2+(size_t)c->hc_mult*c->dim;
    m->boundary_output=out+2;m->boundary_output_mix=out+2+(size_t)c->hc_mult*c->dim;
    forward(m,r->token_ids,1,NULL);out[0]=(float)aux;out[1]=(float)(width-aux);
    int rc=lmb_v41_aux_write(m,s->counts,out+aux,width-aux);
    m->boundary_input=m->boundary_mix=NULL;m->boundary_output=m->boundary_output_mix=NULL;
    pthread_mutex_unlock(&lmb_v41_runtime_lock);
    return rc ? coli_segment_adapter_error(err,size,"V4.1 shared-state encoding failed; discard session") : 0;
}
static void lmb_v41_edge_destroy(void *impl) {
    LmbV41Edge *e=impl;if(!e)return;tok_free(&e->tokenizer);lmb_v41_model_destroy(&e->weights,1);free(e);
}
static int lmb_v41_edge_open(void **impl,ColiEdgeCapabilities *cap,const ColiEdgeEngineOptions *o,char *err,size_t size) {
    *impl=NULL;
    if(o->memory_limit_bytes || o->resource_plan_size)
        return coli_edge_adapter_error(err,size,"V4.1 bounded Edge admission is not implemented; laboratory only");
    if(o->backend_mask && o->backend_mask!=COLI_EDGE_CAP_CPU)
        return coli_edge_adapter_error(err,size,"V4.1 laboratory is CPU only");
    LmbV41Edge *e=calloc(1,sizeof *e);if(!e)return -1;
    Model *m=&e->weights;cfg_load(&m->c,o->model_dir);
    if(!lmb_v41_layout_valid(&m->c)){free(e);return coli_edge_adapter_error(err,size,"invalid V4.1 layout");}
    st_init_multi(&m->S,o->model_dir,NULL);
    wb_load(&m->S,&m->embed,"embed.weight",m->c.vocab,m->c.dim);
    wb_load(&m->S,&m->head,"head.weight",m->c.vocab,m->c.dim);
    wf_load(&m->S,&m->norm,"norm.weight",m->c.dim);
    char path[1024];int n=snprintf(path,sizeof path,"%s/tokenizer.json",o->model_dir);
    if(n<0 || (size_t)n>=sizeof path){lmb_v41_edge_destroy(e);return -1;}
    tok_load(&e->tokenizer,path);e->width=lmb_v41_state_width(&m->c);
    memset(cap,0,sizeof *cap);cap->struct_size=sizeof *cap;cap->abi_version=COLI_EDGE_ABI_VERSION;
    cap->flags=COLI_EDGE_CAP_TOKENIZE|COLI_EDGE_CAP_DETOKENIZE|COLI_EDGE_CAP_GREEDY|COLI_EDGE_CAP_LOGITS|COLI_EDGE_CAP_CPU;
    snprintf(cap->engine_id,sizeof cap->engine_id,"deepseek_v41");
    snprintf(cap->state_schema,sizeof cap->state_schema,LMB_V41_SCHEMA);
    snprintf(cap->numeric_class,sizeof cap->numeric_class,LMB_V41_NUMERIC);
    snprintf(cap->tokenizer_class,sizeof cap->tokenizer_class,"colibri-tok-v1");
    cap->state_dtype=COLI_EDGE_DTYPE_F32;cap->state_width=(uint32_t)e->width;
    cap->vocab_size=(uint32_t)m->c.vocab;cap->max_batch_rows=1;
    cap->max_context_tokens=(uint32_t)m->c.max_positions;cap->num_layers=(uint32_t)m->c.n_layers;
    cap->bos_token_id=-1;cap->eos_token_id=-1;int eos[1];if(serve_eos(m,o->model_dir,eos,1)==1)cap->eos_token_id=eos[0];
    cap->resident_bytes=(uint64_t)m->c.vocab*m->c.dim*4+(uint64_t)m->c.dim*4;
    *impl=e;return 0;
}
static int lmb_v41_tokenize(void *impl,const char *text,size_t n,int32_t *ids,size_t cap,size_t *count,char *err,size_t size) {
    LmbV41Edge *e=impl;return coli_edge_tok_tokenize(&e->tokenizer,text,n,ids,cap,count,err,size);
}
static int lmb_v41_detokenize(void *impl,const int32_t *ids,size_t n,char *text,size_t cap,size_t *count,char *err,size_t size) {
    LmbV41Edge *e=impl;return coli_edge_tok_detokenize(&e->tokenizer,ids,n,text,cap,count,err,size);
}
static int lmb_v41_embed(void *impl,const ColiEdgeEmbedRequest *r,char *err,size_t size) {
    LmbV41Edge *e=impl;const Cfg *c=&e->weights.c;
    if(r->rows!=1 || r->token_count!=1 || r->token_ids[0]<0 || r->token_ids[0]>=c->vocab || r->output_bytes!=e->width*4)
        return coli_edge_adapter_error(err,size,"invalid V4.1 embed input");
    float *row=r->output;memset(row,0,e->width*4);size_t aux=lmb_v41_state_aux(c);
    row[0]=(float)aux;row[1]=(float)(e->width-aux);
    for(int k=0;k<c->hc_mult;k++) {
        for(int j=0;j<c->dim;j++)row[2+(size_t)k*c->dim+j]=bf16_to_f32(e->weights.embed.w[(size_t)r->token_ids[0]*c->dim+j]);
        row[2+(size_t)c->hc_mult*c->dim+k]=k ? 0.f : 1.f;
    }
    int counts[V41_MAX_LAYERS]={0};return lmb_v41_aux_write(&e->weights,counts,row+aux,e->width-aux);
}
static int lmb_v41_logits(void *impl,const ColiEdgeLogitsRequest *r,char *err,size_t size) {
    LmbV41Edge *e=impl;const Cfg *c=&e->weights.c;
    if(r->rows!=1 || r->logits_capacity<(size_t)c->vocab || !lmb_v41_row_valid(c,r->input,r->input_bytes))
        return coli_edge_adapter_error(err,size,"invalid V4.1 head input");
    float *scratch=malloc((size_t)c->dim*8);if(!scratch)return -1;
    const float *row=r->input;
    for(int j=0;j<c->dim;j++) {
        float sum=0;for(int k=0;k<c->hc_mult;k++)sum+=row[2+(size_t)c->hc_mult*c->dim+k]*row[2+(size_t)k*c->dim+j];
        scratch[j]=sum;
    }
    pthread_mutex_lock(&lmb_v41_runtime_lock);
    rms_into(scratch+c->dim,scratch,e->weights.norm.w,c->dim,c->norm_eps);
    mvb(r->logits,&e->weights.head,scratch+c->dim);
    pthread_mutex_unlock(&lmb_v41_runtime_lock);free(scratch);return 0;
}
static int lmb_v41_select(void *impl,const ColiEdgeSelectRequest *r,char *err,size_t size) {
    LmbV41Edge *e=impl;float *logits=malloc((size_t)e->weights.c.vocab*4);if(!logits)return -1;
    ColiEdgeLogitsRequest lr={.struct_size=sizeof lr,.rows=r->rows,.input=r->input,.input_bytes=r->input_bytes,
        .logits=logits,.logits_capacity=(size_t)e->weights.c.vocab};
    int rc=lmb_v41_logits(impl,&lr,err,size);
    if(!rc)rc=coli_edge_argmax(logits,(uint32_t)e->weights.c.vocab,r->token_ids,r->scores);
    free(logits);return rc;
}
static int lmb_v41_lab_register(void) {
    static const ColiSegmentAdapter segment={.struct_size=sizeof segment,.abi_version=COLI_SEGMENT_ABI_VERSION,
        .engine_id="deepseek_v41",.engine_open=lmb_v41_segment_open,.engine_destroy=lmb_v41_segment_destroy,
        .session_create=lmb_v41_session_create,.session_destroy=lmb_v41_session_destroy,.session_run=lmb_v41_session_run};
    static const ColiEdgeAdapter edge={.struct_size=sizeof edge,.abi_version=COLI_EDGE_ABI_VERSION,
        .engine_id="deepseek_v41",.engine_open=lmb_v41_edge_open,.engine_destroy=lmb_v41_edge_destroy,
        .tokenize=lmb_v41_tokenize,.detokenize=lmb_v41_detokenize,.embed=lmb_v41_embed,.select=lmb_v41_select,.logits=lmb_v41_logits};
    return coli_segment_adapter_register(&segment) || coli_edge_adapter_register(&edge);
}
#endif
