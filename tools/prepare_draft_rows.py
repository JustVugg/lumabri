#!/usr/bin/env python3
"""Provisional (draft) rows in disposable Colibri Segment adapter sources.

Never modifies the upstream checkout. Exact anchors fail closed on drift.
Model arithmetic is unchanged: a provisional run executes the same forward,
and a rewind only forgets rows (see engine_patches/segment_draft.h).
"""
from pathlib import Path
from prepare_qwen_variants import once

INCLUDE = '#include "engine_patches/segment_draft.h"\n'

RUNTIME = INCLUDE + '''
int lmb_segment_engine_draft_rows(const struct ColiSegmentEngine *engine) {
    return engine && engine->adapter &&
           engine->adapter->reserved_fn[LMB_SEGMENT_REWIND_SLOT] != NULL;
}

int lmb_segment_session_rewind(struct ColiSegmentSession *session,
                               uint64_t position,
                               char *error, size_t error_size) {
    if (!session || !lmb_segment_engine_draft_rows(session->engine))
        return set_error(error, error_size,
                         "segment engine cannot discard provisional rows");
    if (position > session->context_tokens)
        return set_error(error, error_size, "segment rewind exceeds session context");
    LmbSegmentRewindFn rewind = (LmbSegmentRewindFn)
        session->engine->adapter->reserved_fn[LMB_SEGMENT_REWIND_SLOT];
    return rewind(session->impl, position, error, error_size);
}
'''

OLMOE = '''
/* Attention reads KV rows below kv_len only and every run sets kv_len from
 * the session position, so forgetting rows is the whole rewind. */
static int olmoe_segment_session_rewind(void *session_impl, uint64_t position,
                                        char *error, size_t error_size) {
    OlmoeSegmentSession *session = (OlmoeSegmentSession *)session_impl;
    if (!session || position > session->position)
        return coli_segment_adapter_error(
            error, error_size, "OLMoE Segment cannot rewind forwards");
    session->position = (uint32_t)position;
    return 0;
}

'''

QWEN38 = INCLUDE + '''
/* DeltaNet and convolution state is recurrent: it cannot be recomputed
 * backwards, so a provisional run keeps one copy of it after every row but
 * the last. KV/index rows are position-indexed and need no copy. */
static size_t lmb_q38_draft_cells(const Qwen38SegmentSession *s) {
    const Cfg *c=&s->engine->model.c; size_t cells=0;
    for(uint32_t i=s->engine->layer_begin;i<s->engine->layer_end;i++) if(!c->is_attn[i])
        cells+=(size_t)c->dn_vheads*c->dn_kdim*c->dn_vdim+(size_t)c->dn_conv_dim*(size_t)(c->dn_convk-1);
    if(s->PLE_conv_state) cells+=(size_t)c->hc_width*(size_t)(c->ple_convk-1)*(size_t)c->ngram_size;
    return cells;
}
static void lmb_q38_draft_copy(Qwen38SegmentSession *s,LmbQ38DraftRow *row,int save) {
    const Cfg *c=&s->engine->model.c; float *at=row->cells;
#define LMB_Q38_DRAFT_SPAN(pointer,count) do { size_t n=(size_t)(count)*sizeof(float); \\
        if(save) memcpy(at,(pointer),n); else memcpy((pointer),at,n); at+=(count); } while(0)
    for(uint32_t i=s->engine->layer_begin;i<s->engine->layer_end;i++) if(!c->is_attn[i]) {
        LMB_Q38_DRAFT_SPAN(s->DN_rec[i],(size_t)c->dn_vheads*c->dn_kdim*c->dn_vdim);
        LMB_Q38_DRAFT_SPAN(s->DN_conv[i],(size_t)c->dn_conv_dim*(size_t)(c->dn_convk-1));
    }
    if(s->PLE_conv_state)
        LMB_Q38_DRAFT_SPAN(s->PLE_conv_state,(size_t)c->hc_width*(size_t)(c->ple_convk-1)*(size_t)c->ngram_size);
#undef LMB_Q38_DRAFT_SPAN
    if(save){memcpy(row->ple_history,s->ple_history,sizeof row->ple_history);row->ple_history_len=s->ple_history_len;}
    else{memcpy(s->ple_history,row->ple_history,sizeof row->ple_history);s->ple_history_len=row->ple_history_len;}
}
static void lmb_q38_draft_free(Qwen38SegmentSession *s) {
    for(uint32_t i=0;i<s->lmb_draft_capacity;i++) free(s->lmb_draft[i].cells);
    free(s->lmb_draft); s->lmb_draft=NULL; s->lmb_draft_capacity=0; s->lmb_draft_rows=0;
}
static int lmb_q38_draft_reserve(Qwen38SegmentSession *s,uint32_t rows) {
    if(rows<=s->lmb_draft_capacity) return 0;
    LmbQ38DraftRow *grown=(LmbQ38DraftRow*)realloc(s->lmb_draft,(size_t)rows*sizeof(*grown));
    if(!grown) return -1;
    s->lmb_draft=grown;
    size_t cells=lmb_q38_draft_cells(s);
    while(s->lmb_draft_capacity<rows) {
        float *state=(float*)malloc((cells?cells:1)*sizeof(float));
        if(!state) return -1;
        memset(&s->lmb_draft[s->lmb_draft_capacity],0,sizeof(*grown));
        s->lmb_draft[s->lmb_draft_capacity++].cells=state;
    }
    return 0;
}
static int qwen38_segment_session_rewind(void *impl,uint64_t position,char *error,size_t error_size) {
    Qwen38SegmentSession *s=(Qwen38SegmentSession*)impl;
    if(!s) return coli_segment_adapter_error(error,error_size,"invalid Qwen3.8 Segment rewind");
    if(position==s->position) return 0;
    /* lmb_draft[j] is the state after row j of the provisional run. */
    if(!s->lmb_draft_rows||position<=s->lmb_draft_base||position>=s->position||
       position-s->lmb_draft_base>s->lmb_draft_rows)
        return coli_segment_adapter_error(error,error_size,"Qwen3.8 Segment has no state copy for that row");
    lmb_q38_draft_copy(s,&s->lmb_draft[position-s->lmb_draft_base-1],0);
    s->position=(uint32_t)position;
    return 0;
}

'''


def prepare(root):
    root = Path(root)
    path = root / 'segment_runtime.c'
    path.write_text(path.read_text() + RUNTIME)

    path = root / 'olmoe.c'
    s = path.read_text()
    s = once(s, 'static int olmoe_segment_payload_size(const OlmoeSegmentSession *session,',
             OLMOE + 'static int olmoe_segment_payload_size(const OlmoeSegmentSession *session,')
    s = once(s, 'static int olmoe_segment_session_run(void *session_impl,',
             INCLUDE + 'static int olmoe_segment_session_run(void *session_impl,')
    # A provisional block forwards one row at a time, the forward a decode
    # step uses, so each kept row is bit-for-bit the row plain decode computes.
    s = once(s, '''    layers_forward_range(model, (float *)request->output, (int)request->rows,
                         (int)request->position, (int)engine->layer_begin,
                         (int)engine->layer_end, 0);
    model->K = NULL; model->V = NULL; model->max_t = 0; model->kv_len = 0;
    pthread_mutex_unlock(&engine->run_lock);
    session->position += request->rows;''', '''    if (request->reserved_u64[0] == LMB_SEGMENT_RUN_PROVISIONAL) {
        size_t width = request->input_bytes / sizeof(float) / request->rows;
        for (uint32_t row = 0; row < request->rows; row++) {
            model->kv_len = (int)(session->position + row);
            layers_forward_range(model, (float *)request->output + (size_t)row * width, 1,
                                 (int)(request->position + row), (int)engine->layer_begin,
                                 (int)engine->layer_end, 0);
        }
    } else
    layers_forward_range(model, (float *)request->output, (int)request->rows,
                         (int)request->position, (int)engine->layer_begin,
                         (int)engine->layer_end, 0);
    model->K = NULL; model->V = NULL; model->max_t = 0; model->kv_len = 0;
    pthread_mutex_unlock(&engine->run_lock);
    session->position += request->rows;''')
    s = once(s, '    olmoe_segment_session_restore, {0}\n};',
             '    olmoe_segment_session_restore,\n'
             '    {(void (*)(void))olmoe_segment_session_rewind}\n};')
    path.write_text(s)

    path = root / 'qwen38.c'
    s = path.read_text()
    s = once(s, '''    float *PLE_conv_state;
    uint32_t context_tokens, position;
} Qwen38SegmentSession;''', '''    float *PLE_conv_state;
    uint32_t context_tokens, position;
    struct LmbQ38DraftRow *lmb_draft;
    uint32_t lmb_draft_capacity, lmb_draft_rows, lmb_draft_base;
} Qwen38SegmentSession;''')
    s = once(s, '''typedef struct {
    Qwen38SegmentEngine *engine;
    float **K, **V, **IK, **DN_rec, **DN_conv;''', '''typedef struct LmbQ38DraftRow {
    float *cells; int64_t ple_history[2]; int ple_history_len;
} LmbQ38DraftRow;
typedef struct {
    Qwen38SegmentEngine *engine;
    float **K, **V, **IK, **DN_rec, **DN_conv;''')
    anchor = 'static void qwen38_segment_session_state_free(Qwen38SegmentSession *s) {\n    if (!s) return;'
    s = once(s, anchor, 'static void lmb_q38_draft_free(Qwen38SegmentSession *s);\n' + anchor +
             '\n    lmb_q38_draft_free(s);')
    anchor = 'static int qwen38_segment_session_run(void *impl,const ColiSegmentRunRequest *r,char *error,size_t error_size){'
    s = once(s, anchor, QWEN38 + anchor)
    # A provisional run forwards one row at a time, exactly as decode does,
    # and copies the recurrent state between rows.
    s = once(s, '''    q38_layers_forward_range(m,(float*)r->output,r->token_ids,(int)r->rows,(int)r->position,(int)e->layer_begin,(int)e->layer_end);
    s->ple_history_len=m->ple_history_len;''', '''    if(provisional) {
        for(uint32_t row=0;row<r->rows;row++) {
            m->kv_len=(int)(r->position+row);
            q38_layers_forward_range(m,(float*)r->output+(size_t)row*c->hc_width,r->token_ids+row,1,(int)(r->position+row),(int)e->layer_begin,(int)e->layer_end);
            s->ple_history_len=m->ple_history_len;
            if(row+1<r->rows) lmb_q38_draft_copy(s,&s->lmb_draft[row],1);
        }
        s->lmb_draft_base=(uint32_t)r->position; s->lmb_draft_rows=r->rows-1;
    } else {
        s->lmb_draft_rows=0;
        q38_layers_forward_range(m,(float*)r->output,r->token_ids,(int)r->rows,(int)r->position,(int)e->layer_begin,(int)e->layer_end);
    }
    s->ple_history_len=m->ple_history_len;''')
    s = once(s, '''    if(r->output!=r->input)memcpy(r->output,r->input,bytes);
    pthread_mutex_lock(&e->run_lock); Model *m=&e->model;''', '''    int provisional=r->reserved_u64[0]==LMB_SEGMENT_RUN_PROVISIONAL;
    if(provisional&&lmb_q38_draft_reserve(s,r->rows))return coli_segment_adapter_error(error,error_size,"out of memory keeping Qwen3.8 draft state");
    if(r->output!=r->input)memcpy(r->output,r->input,bytes);
    pthread_mutex_lock(&e->run_lock); Model *m=&e->model;''')
    s = once(s, '    s->position=h.position;return 0;\n}',
             '    s->position=h.position;s->lmb_draft_rows=0;return 0;\n}')
    s = once(s, 'qwen38_segment_session_run,qwen38_segment_session_snapshot,qwen38_segment_session_restore,{0}};',
             'qwen38_segment_session_run,qwen38_segment_session_snapshot,qwen38_segment_session_restore,'
             '{(void (*)(void))qwen38_segment_session_rewind}};')
    path.write_text(s)
