"""Connect native V4 GGUF weights to resident, model-owned adapter state."""
from pathlib import Path
from prepare_qwen_variants import once


def prepare(root):
    path=Path(root)/'deepseek_v4.c';s=path.read_text()
    anchor='int coli_st_index_open(ColiSafetensorsIndex **out, const char *directory,'
    s=once(s,anchor,'static pthread_mutex_t lmb_v4_index_lock=PTHREAD_MUTEX_INITIALIZER;\n\n'+anchor)
    s=once(s,'    st_init_multi(index, directory, (extra_dirs && *extra_dirs) ? extra_dirs : NULL);',
        '''    pthread_mutex_lock(&lmb_v4_index_lock);
    st_gguf_active=0;
    st_init_multi(index, directory, (extra_dirs && *extra_dirs) ? extra_dirs : NULL);
    gguf_close(&st_gguf_meta);gguf_close(&st_gguf_head);st_gguf_active=0;
    pthread_mutex_unlock(&lmb_v4_index_lock);''')
    anchor='''static uint64_t deepseek_v4_segment_expert_record_bytes(
    const ColiSafetensorsIndex *index) {'''
    s=once(s,anchor,anchor+'''
    const ColiSafetensorsTensor *first=coli_st_find(index,"layers.0.ffn.experts.0.w1.weight");
    if(first && st_is_ggml(first->dtype)) {
        uint64_t largest=0;
        for(int layer=0;;layer++) {
            uint64_t bytes=0;
            for(int part=1;part<=3;part++) {
                char name[128];snprintf(name,sizeof name,"layers.%d.ffn.experts.0.w%d.weight",layer,part);
                const ColiSafetensorsTensor *t=coli_st_find(index,name);
                if(!t)return part==1 && layer>0?largest:0;
                if(t->rank!=2 || !st_is_ggml(t->dtype))return 0;
                int type=coli_v4_gguf_held_type(st_ggml_type(t->dtype),t->shape[0],t->shape[1]);
                int64_t row=gq_row_bytes(type,t->shape[1]);
                if(row<=0 || t->shape[0]<=0 || (uint64_t)t->shape[0]>UINT64_MAX/(uint64_t)row)return 0;
                uint64_t size=(uint64_t)row*(uint64_t)t->shape[0];
                if(bytes>UINT64_MAX-size)return 0;bytes+=size;
            }
            if(bytes>largest)largest=bytes;
        }
    }''')
    a=s.index('static int deepseek_v4_segment_engine_open(');b=s.index('\nfail:',a)
    part=s[a:b];part=once(part,'"deepseek-v4/fp8-mxfp4-bf16/f32/cpu-v1"',
        'coli_v4_index_gguf(model->target_index)?"deepseek-v4/ggml-bf16/f32/cpu-v1":"deepseek-v4/fp8-mxfp4-bf16/f32/cpu-v1"')
    part=once(part,'    capabilities->state_dtype = COLI_SEGMENT_DTYPE_F32;',
        '''    if(coli_v4_index_gguf(model->target_index)) {
        size_t used=strlen(capabilities->numeric_class);
        const char *rows=getenv("COLI_V4_ROWS16"),*identity=getenv("V4_IDX_IDENTITY");
        snprintf(capabilities->numeric_class+used,sizeof(capabilities->numeric_class)-used,"/g%d-r%d-i%d",
            gq_exact_mode(),!rows || atoi(rows)!=0,identity && *identity=='1');
    }
    capabilities->state_dtype = COLI_SEGMENT_DTYPE_F32;''')
    s=s[:a]+part+s[b:]
    s=once(s,'    uint32_t state_width;\n} DeepSeekV4EdgeEngine;',
        '''    uint32_t state_width;
    unsigned char *resident_embed,*resident_head;
    int embed_type,head_type;
    int64_t embed_row,head_row;
} DeepSeekV4EdgeEngine;''')
    a=s.index('static void deepseek_v4_edge_engine_destroy(');b=s.index('\nstatic int deepseek_v4_edge_engine_open(',a)
    part=s[a:b];part=once(part,'    tok_free(&engine->tokenizer);',
        '    free(engine->resident_embed);free(engine->resident_head);\n    tok_free(&engine->tokenizer);')
    s=s[:a]+part+s[b:]
    a=s.index('static int deepseek_v4_edge_engine_open(');b=s.index('\nstatic int deepseek_v4_edge_tokenize(',a)
    part=s[a:b];start=part.index('    if (!embedding || embedding->dtype != COLI_ST_BF16 ||')
    end=part.index('    char tokenizer_path[4096];',start)
    part=part[:start]+'''    if(!embedding || !head || embedding->rank!=2 || head->rank!=2 ||
       embedding->numel!=(int64_t)boundary_cells || head->numel!=(int64_t)boundary_cells) {
        deepseek_v4_edge_engine_destroy(engine);
        return coli_edge_adapter_error(error,error_size,"invalid V4 embedding/head geometry");
    }
    engine->embed_type=st_ggml_type_any(embedding->dtype);engine->head_type=st_ggml_type_any(head->dtype);
    engine->embed_row=gq_row_bytes(engine->embed_type,hidden);engine->head_row=gq_row_bytes(engine->head_type,hidden);
    if(engine->embed_row<=0 || engine->head_row<=0 ||
       embedding->nbytes!=engine->embed_row*engine->config.vocab_size ||
       head->nbytes!=engine->head_row*engine->config.vocab_size) {
        deepseek_v4_edge_engine_destroy(engine);
        return coli_edge_adapter_error(error,error_size,"unsupported V4 embedding/head type");
    }
'''+part[end:]
    start=part.index('    if (getenv("LUMABRI_RESIDENT_REQUIRED")');end=part.index('    memset(capabilities,',start)
    part=part[:start]+'''    const ColiSafetensorsTensor *tables[]={embedding,head};
    unsigned char **copies[]={&engine->resident_embed,&engine->resident_head};
    for(unsigned i=0;i<2;i++) {
        uint64_t bytes=(uint64_t)tables[i]->nbytes;
        if(bytes>SIZE_MAX || resident>UINT64_MAX-bytes ||
           (options->memory_limit_bytes && resident+bytes>options->memory_limit_bytes)) {
            deepseek_v4_edge_engine_destroy(engine);
            return coli_edge_adapter_error(error,error_size,"resident V4 Edge exceeds approved RAM");
        }
        *copies[i]=malloc((size_t)bytes);
        if(!*copies[i] || coli_st_read_tensor(engine->index,tables[i],*copies[i])) {
            deepseek_v4_edge_engine_destroy(engine);
            return coli_edge_adapter_error(error,error_size,"cannot load resident V4 embedding/head");
        }
        resident+=bytes;
    }
'''+part[end:]
    part=once(part,'"deepseek-v4/fp8-mxfp4-bf16/f32/cpu-v1"',
        'coli_v4_index_gguf(engine->index)?"deepseek-v4/ggml-bf16/f32/cpu-v1":"deepseek-v4/fp8-mxfp4-bf16/f32/cpu-v1"')
    part=once(part,'    capabilities->state_dtype = COLI_EDGE_DTYPE_F32;',
        '''    if(coli_v4_index_gguf(engine->index)) {
        size_t used=strlen(capabilities->numeric_class);
        const char *rows=getenv("COLI_V4_ROWS16"),*identity=getenv("V4_IDX_IDENTITY");
        snprintf(capabilities->numeric_class+used,sizeof(capabilities->numeric_class)-used,"/g%d-r%d-i%d",
            gq_exact_mode(),!rows || atoi(rows)!=0,identity && *identity=='1');
    }
    capabilities->state_dtype = COLI_EDGE_DTYPE_F32;''')
    s=s[:a]+part+s[b:]
    a=s.index('static int deepseek_v4_edge_embed(');b=s.index('\nstatic void deepseek_v4_edge_final_hidden(',a)
    s=s[:a]+'''static int deepseek_v4_edge_embed(void *impl,const ColiEdgeEmbedRequest *r,char *error,size_t error_size) {
    DeepSeekV4EdgeEngine *e=impl;int h=e->config.hidden_size;float *out=r->output;
    for(uint32_t i=0;i<r->rows;i++) {
        int token=r->token_ids[i];float *row=out+(size_t)i*e->state_width;
        if(token<0 || token>=e->config.vocab_size ||
           gq_dequant_row(e->embed_type,e->resident_embed+(uint64_t)token*e->embed_row,row,h))
            return coli_edge_adapter_error(error,error_size,"invalid V4 resident embedding");
        for(int copy=1;copy<e->config.hc_mult;copy++)memcpy(row+(size_t)copy*h,row,(size_t)h*sizeof(float));
    }
    return 0;
}
'''+s[b:]
    a=s.index('static int deepseek_v4_edge_argmax(');b=s.index('\nstatic int deepseek_v4_edge_select(',a)
    s=s[:a]+'''static int lmb_v4_edge_scores(DeepSeekV4EdgeEngine *e,const float *hidden,float *scores,
        ColiEdgeCancelFn cancel,void *opaque) {
    int d=e->config.hidden_size,vocab=e->config.vocab_size;
    for(int start=0;start<vocab;start+=1024) {
        if(cancel && cancel(opaque))return -2;
        int n=vocab-start<1024?vocab-start:1024;
        const unsigned char *raw=e->resident_head+(uint64_t)start*e->head_row;
        if(e->head_type==GQ_BF16) {
            #pragma omp parallel for schedule(static)
            for(int i=0;i<n;i++)scores[start+i]=deepseek_v4_edge_head_dot((const uint16_t*)raw+(size_t)i*d,hidden,d);
        } else {
            ColiTensorView view={COLI_TENSOR_GGML,COLI_SCALE_NONE,raw,NULL,(size_t)n*e->head_row,0,n,d,1,(uint32_t)e->head_type,NULL};
            if(coli_v4_ggml_mm(scores+start,&view,hidden,1))return -1;
        }
    }
    return 0;
}
static int deepseek_v4_edge_argmax(DeepSeekV4EdgeEngine *engine,const float *hidden,
        int32_t *best_token,float *best_logit,ColiEdgeCancelFn cancel,void *opaque) {
    int vocab=engine->config.vocab_size;float *scores=malloc((size_t)vocab*sizeof(float));
    if(!scores)return -1;
    int rc=lmb_v4_edge_scores(engine,hidden,scores,cancel,opaque),best=0;
    if(!rc) {
        for(int i=1;i<vocab;i++)if(scores[i]>scores[best])best=i;
        *best_token=best;if(best_logit)*best_logit=scores[best];
    }
    free(scores);return rc;
}
'''+s[b:]
    a=s.index('static int deepseek_v4_edge_logits(');b=s.index('\nstatic const ColiEdgeAdapter deepseek_v4_edge_adapter',a)
    s=s[:a]+'''static int deepseek_v4_edge_logits(void *impl,const ColiEdgeLogitsRequest *r,char *error,size_t error_size) {
    DeepSeekV4EdgeEngine *e=impl;int h=e->config.hidden_size;float *final=malloc((size_t)h*sizeof(float));
    if(!final)return coli_edge_adapter_error(error,error_size,"out of memory computing V4 logits");
    int rc=0;const float *input=r->input;
    for(uint32_t i=0;i<r->rows && !rc;i++) {
        deepseek_v4_edge_final_hidden(e,final,input+(size_t)i*e->state_width);
        rc=lmb_v4_edge_scores(e,final,r->logits+(size_t)i*e->config.vocab_size,r->should_cancel,r->cancel_user_data);
    }
    free(final);
    return rc?coli_edge_adapter_error(error,error_size,rc==-2?"V4 logits cancelled":"V4 logits failed"):0;
}
'''+s[b:]
    path.write_text(s)


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--copy',required=True,type=Path)
    prepare(p.parse_args().copy)
