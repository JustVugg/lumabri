"""Mistral Large 4 shared layer state in disposable Colibri source copies."""
from pathlib import Path
from prepare_qwen_variants import once


def prepare(root):
    path=Path(root)/'colibri.c'
    s=path.read_text()
    s=once(s,'    int *dsa_sel, *dsa_nsel; int dsa_scap;',
        '    const float *lmb_ml4_cross[128];\n    int *dsa_sel, *dsa_nsel; int dsa_scap;')
    s=once(s,'typedef struct { int pos,qk,valid,inv_valid; float theta,inv_theta,inv[128],cs[128],sn[128]; } RopeCache;',
        'typedef struct { int pos,qk,valid,inv_valid,inv_mode; float theta,inv_theta,inv[128],cs[128],sn[128]; } RopeCache;')
    s=once(s,'if(!cache.inv_valid||cache.qk!=c->qk_rope||cache.inv_theta!=c->theta){',
        'if(!cache.inv_valid||cache.qk!=c->qk_rope||cache.inv_theta!=c->theta||cache.inv_mode!=c->rope_inv_on||\n'
        '       (c->rope_inv_on && memcmp(cache.inv,c->rope_inv,(size_t)half*sizeof(float)))){')
    s=once(s,'cache.qk=c->qk_rope; cache.inv_theta=c->theta; cache.inv_valid=1;',
        'cache.qk=c->qk_rope; cache.inv_theta=c->theta; cache.inv_valid=1; cache.inv_mode=c->rope_inv_on;')
    s=once(s,'memcpy(ff,f,half*sizeof(float)); has_ff=1;',
        'memcpy(ff,f,half*sizeof(float)); free(f); has_ff=1;')
    s=once(s,'ml4_row(ks,src,pos,kvl,0,b1)',
        '(m->lmb_ml4_cross[src]?m->lmb_ml4_cross[src]:ml4_row(ks,src,pos,kvl,0,b1))')
    s=once(s,'ml4_row(ks,src,pos,RQ,1,b2)',
        '(m->lmb_ml4_cross[src]?m->lmb_ml4_cross[src]+kvl:ml4_row(ks,src,pos,RQ,1,b2))')
    anchor='#ifdef COLI_SEGMENT_ADAPTER\n/* ---------- engine-owned Segment adapter'
    s=once(s,anchor,'''#if defined(COLI_SEGMENT_ADAPTER) || defined(COLI_EDGE_ADAPTER)
#include "engine_patches/mistral_boundary.h"
static pthread_mutex_t lmb_glm_open_lock=PTHREAD_MUTEX_INITIALIZER;
static void lmb_glm_source_release(void) {
    gguf_close(&st_gguf_meta);gguf_close(&st_gguf_head);st_gguf_active=0;
}
#endif
'''+anchor)
    # Native GGUF kv_b owns two matrices. Mistral also owns SSMax and query
    # index norms that the older GLM lifecycle never needed to destroy.
    for name in ('glm_segment_qt_destroy','glm_edge_qt_destroy'):
        a=s.index('static void '+name+'(');b=s.index('\n}',a)
        part=s[a:b]
        part=once(part,'    free(tensor->qf);',
            '    if(tensor->kvb) {\n        '+name+'(&tensor->kvb->kb);\n        '+name+'(&tensor->kvb->vb);\n        free(tensor->kvb);\n    }\n    free(tensor->qf);')
        s=s[:a]+part+s[b:]
    s=once(s,'    free(layer->q_a_ln); free(layer->kv_a_ln);',
        '    free(layer->q_a_ln); free(layer->kv_a_ln);\n    free(layer->ssm_s);free(layer->ssm_b);')
    s=once(s,'            free(model->ix_knw[layer]); free(model->ix_knb[layer]);',
        '''            free(model->ix_knw[layer]); free(model->ix_knb[layer]);
            if(model->c.ml4) {
                free(model->ix_qnw[layer]);free(model->ix_qnb[layer]);
                free(model->ix_ssm_s[layer]);free(model->ix_ssm_b[layer]);
            }''')
    s=once(s,'    free(model->ix_knw); free(model->ix_knb);',
        '    free(model->ix_knw); free(model->ix_knb);\n    free(model->ix_qnw);free(model->ix_qnb);free(model->ix_ssm_s);free(model->ix_ssm_b);')
    # Match model-specific wire width and numerical contract at both ends.
    for kind in ('segment','edge'):
        a=s.index('static int glm_'+kind+'_engine_open(')
        b=s.index('\nstatic ',a+1)
        part=s[a:b]
        cfg='config' if kind=='segment' else 'model->c'
        part=part.replace('"glm/mla-rope-dsa-f32-v1"',cfg+'.ml4 ? "mistral-large4/latent-selection-f32-v1" : "glm/mla-rope-dsa-f32-v1"')
        part=part.replace('"glm/e%d-d%d-dsa%d-kvf32/cpu-v1"',cfg+'.ml4 ? "mistral-large4/e%d-d%d-dsa%d-kvf32/cpu-v1" : "glm/e%d-d%d-dsa%d-kvf32/cpu-v1"')
        part=once(part,'capabilities->state_width = (uint32_t)'+cfg+'.hidden;',
            'capabilities->state_width = (uint32_t)lmb_ml4_width(&'+cfg+');')
        s=s[:a]+part+s[b:]
    a=s.index('static int glm_segment_session_run(');b=s.index('\nstatic int glm_segment_spans(',a)
    part=s[a:b]
    part=once(part,'    if (request->output != request->input)',
        '''    if(engine->model.c.ml4) {
        Model *m=&engine->model;size_t width=lmb_ml4_width(&m->c);
        const float *input=request->input;float *output=request->output;
        for(uint32_t i=0;i<request->rows;i++)
            if(lmb_ml4_validate(m,input+(size_t)i*width,(int)engine->layer_begin,(int)request->position+(int)i))
                return coli_segment_adapter_error(error,error_size,"invalid Mistral cross-layer state");
        pthread_mutex_lock(&engine->run_lock);kv_bind(m,&session->kv);
        int bad=0;
        for(uint32_t i=0;i<request->rows;i++) {
            float *row=output+(size_t)i*width;
            if(row!=input+(size_t)i*width)memcpy(row,input+(size_t)i*width,width*sizeof(float));
            if(lmb_ml4_input(m,row,(int)engine->layer_begin)){bad=1;break;}
            layers_forward_range(m,row,1,(int)request->position+(int)i,
                (int)engine->layer_begin,(int)engine->layer_end);
            lmb_ml4_output(m,row,(int)engine->layer_begin,(int)engine->layer_end,(int)request->position+(int)i);
        }
        memset(m->lmb_ml4_cross,0,sizeof m->lmb_ml4_cross);
        kv_bind(m,engine->base_kv);pthread_mutex_unlock(&engine->run_lock);
        if(bad)return coli_segment_adapter_error(error,error_size,"cannot allocate Mistral sparse state");
        session->position+=request->rows;return 0;
    }
    if (request->output != request->input)''')
    s=s[:a]+part+s[b:]
    a=s.index('static int glm_edge_embed(');b=s.index('static const ColiEdgeAdapter glm_edge_adapter',a)
    part=s[a:b]
    part=once(part,'    float *output = (float *)request->output;',
        '    size_t width=lmb_ml4_width(&engine->model.c);\n    float *output = (float *)request->output;')
    part=once(part,'        embed_row(&engine->model, token, output + (size_t)row * hidden);',
        '        memset(output+(size_t)row*width,0,width*sizeof(float));\n        embed_row(&engine->model, token, output + (size_t)row * width);')
    part=once(part,'    int hidden = engine->model.c.hidden;','')
    part=part.replace('input + (size_t)row * config->hidden','input + (size_t)row * lmb_ml4_width(config)')
    part=once(part,'matmul_qt(logits, normalized, &engine->model.lm_head, 1);',
        'lm_logits(&engine->model,logits,normalized,1);')
    part=once(part,'matmul_qt(request->logits + (size_t)row * config->vocab,\n                  normalized, &engine->model.lm_head, 1);',
        'lm_logits(&engine->model,request->logits+(size_t)row*config->vocab,normalized,1);')
    s=s[:a]+part+s[b:]
    # st_init indexes its own descriptors. The global GGUF header is needed
    # only while opening; serialize opens and release it on every return.
    for kind in ('segment','edge'):
        prefix='glm_'+kind;title=kind.title()
        a=s.index('static int '+prefix+'_engine_open(')
        s=s[:a]+s[a:].replace(prefix+'_engine_open(',prefix+'_engine_open_inner(',1)
        at=s.index('static const Coli'+title+'Adapter '+prefix+'_adapter')
        wrapper=f'''static int {prefix}_engine_open(void **out,Coli{title}Capabilities *caps,
    const Coli{title}EngineOptions *options,char *error,size_t capacity) {{
    pthread_mutex_lock(&lmb_glm_open_lock);
    int rc={prefix}_engine_open_inner(out,caps,options,error,capacity);
    lmb_glm_source_release();pthread_mutex_unlock(&lmb_glm_open_lock);
    return rc;
}}

'''
        s=s[:at]+wrapper+s[at:]
    path.write_text(s)


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--copy',required=True,type=Path)
    prepare(p.parse_args().copy)
