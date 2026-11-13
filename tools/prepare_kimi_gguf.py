"""Connect Kimi's native GGUF boundary weights in disposable engine copies."""
from pathlib import Path
from prepare_qwen_variants import once


def prepare(root):
    path=Path(root)/'kimi_k3.c';s=path.read_text()
    s=once(s,'    free(buf); (void)arena;', '    json_free(root); free(buf); free(arena);')
    anchor='static void model_init_range(Model *m, const char *snap, int layer_begin,'
    s=once(s,anchor,'''/* Resident execution owns every expert. Budget the kernel workspace instead
 * of reserving page cache for a streaming path that cannot run here. */
static double lmb_k3_resident_workspace(const Model *m) {
    const Cfg *c=&m->c;
    uint64_t boundary=(uint64_t)c->hidden*(1u+(c->n_layers+c->res_bs-1u)/c->res_bs);
    uint64_t wide=boundary+(uint64_t)c->n_heads*(c->qk_nope+c->qk_rope+2u*c->v_head)+
        (uint64_t)c->kda_proj*(c->kda_hd+8u)+(uint64_t)c->n_shared*c->moe_inter+
        c->dense_inter+c->latent+c->q_lora+c->kv_lora+(uint64_t)c->index_hd*(c->index_nh+1u)+
        c->n_experts+(uint64_t)c->topk*(c->latent+c->moe_inter);
    return (double)wide*(128u*16u+1024u)*4u/1e9;
}
'''+anchor)
    s=once(s,'        double reserve = 2.5 + 1.2 + kv_gb + ckpt_gb;',
        '''        int lmb_resident=segment_mode && getenv("LUMABRI_RESIDENT_REQUIRED") &&
            !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"),"1");
        double reserve = (lmb_resident?lmb_k3_resident_workspace(m):2.5+1.2) + kv_gb + ckpt_gb;''')
    s=once(s,'    int has_head;', '    int has_head, lmb_gguf;')
    s=once(s,'    st_init_multi(&m->S,snap,getenv("K3_DIRS"));',
        '    st_init_multi(&m->S,snap,getenv("K3_DIRS"));\n    m->lmb_gguf=st_gguf_active;')
    s=once(s,'    st_init_multi(&model->S, options->model_dir, getenv("K3_DIRS"));',
        '    st_init_multi(&model->S, options->model_dir, getenv("K3_DIRS"));\n    model->lmb_gguf=st_gguf_active;')
    s=once(s,'    Tok tokenizer;\n    uint32_t state_width, nbmax;',
        '    Tok tokenizer;\n    float *lmb_embedding;\n    uint32_t state_width, nbmax;')
    s=once(s,'    free(engine->model.final_norm); free(engine->model.out_sw);',
        '    free(engine->lmb_embedding);\n    free(engine->model.final_norm); free(engine->model.out_sw);')
    # Complete the existing destructor for GGUF table metadata and model-local
    # expert telemetry. They must not accumulate across unloaded allocations.
    s=once(s,'    free(model->eref); free(model->ecache); free(model->L);',
        '''    free(model->gx_t); free(model->gx_len);
    if(model->ehit) {
        for(int i=0;i<model->c.n_layers;i++)free(model->ehit[i]);
        free(model->ehit);
    }
    free(model->eref); free(model->ecache); free(model->L);''')
    at=s.index('    uint64_t resident = kimi_edge_w_bytes')
    s=s[:at]+'''    if(getenv("LUMABRI_RESIDENT_REQUIRED") && !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"),"1")) {
        uint64_t bytes=(uint64_t)config->hidden*config->vocab*sizeof(float);
        if(!options->memory_limit_bytes || bytes>=options->memory_limit_bytes ||
            lmb_resident_budget_exceeded(options->memory_limit_bytes-bytes)) {
            kimi_edge_engine_destroy(engine);
            return coli_edge_adapter_error(error,error_size,"Kimi embedding exceeds approved RAM");
        }
        engine->lmb_embedding=f32_load(model,"model.embed_tokens.weight",(int64_t)config->hidden*config->vocab);
    }
'''+s[at:]
    s=once(s,'                        2u * (uint64_t)config->hidden * sizeof(float);',
        '''                        2u * (uint64_t)config->hidden * sizeof(float);
    if(engine->lmb_embedding)resident+=(uint64_t)config->hidden*config->vocab*sizeof(float);''')
    s=once(s,'''        st_read_slice_f32(&engine->model.S, name,
                          (int64_t)token * config->hidden,
                          config->hidden, state, 0);''',
        '''        if(engine->lmb_embedding)
            memcpy(state,engine->lmb_embedding+(size_t)token*config->hidden,config->hidden*sizeof(float));
        else st_read_slice_f32(&engine->model.S, name,
                          (int64_t)token * config->hidden,
                          config->hidden, state, 0);''')
    a=s.index('    float *residual_norm = f32_load(',s.index('static int kimi_edge_engine_open('))
    b=s.index('    w_load(model, &model->lm_head',a)
    s=s[:a]+'''    if (k3_has(model,"model.output_attn_res_score.weight")) {
        model->out_sw=f32_load(model,"model.output_attn_res_score.weight",config->hidden);
    } else {
'''+s[a:b]+'''    }
'''+s[b:]
    s=once(s,'    uint64_t cells = (uint64_t)weight->O * weight->I;',
        '''    if (weight->fmt>=100) {
        int64_t row=gq_row_bytes(weight->fmt-100,weight->I);
        return row<0?UINT64_MAX:(uint64_t)row*weight->O;
    }
    uint64_t cells = (uint64_t)weight->O * weight->I;''')
    for kind in ('segment','edge'):
        a=s.index('static int kimi_'+kind+'_engine_open(');b=s.index('    *engine_impl = engine;',a)
        part=s[a:b];needle='"kimi-k3/q%d-mla%d-mxfp4-idot%d/f32/cpu-v1",'
        part=once(part,needle,'engine->model.lmb_gguf ? "kimi-k3/q%d-mla%d-head%d-gguf-idot%d/f32/cpu-v1" :\n             "kimi-k3/q%d-mla%d-head%d-mxfp4-idot%d/f32/cpu-v1",')
        part=once(part,'bits, mla_bits, g_k3_idot != 0',
            'bits, mla_bits, getenv("K3_HEAD_BITS")?atoi(getenv("K3_HEAD_BITS")):8, g_k3_idot != 0')
        s=s[:a]+part+s[b:]
    # The GGUF source table is global in the pinned loader. Serialize only
    # opens; inference uses engine-owned weights and session-owned state.
    anchor='#ifdef COLI_SEGMENT_ADAPTER\n'
    a=s.index(anchor,s.index('int main('))
    s=s[:a]+'''#if defined(COLI_SEGMENT_ADAPTER) || defined(COLI_EDGE_ADAPTER)
static pthread_mutex_t lmb_k3_open_lock=PTHREAD_MUTEX_INITIALIZER;
static void lmb_k3_source_release(void) {
    gguf_close(&st_gguf_meta);gguf_close(&st_gguf_head);st_gguf_active=0;
}
#endif
'''+s[a:]
    for kind in ('segment','edge'):
        prefix='kimi_'+kind;title=kind.title()
        a=s.index('static int '+prefix+'_engine_open(')
        s=s[:a]+s[a:].replace(prefix+'_engine_open(',prefix+'_engine_open_inner(',1)
        at=s.index('static const Coli'+title+'Adapter '+prefix+'_adapter')
        wrapper=f'''static int {prefix}_engine_open(void **out,Coli{title}Capabilities *caps,
    const Coli{title}EngineOptions *options,char *error,size_t capacity) {{
    pthread_mutex_lock(&lmb_k3_open_lock);
    int rc={prefix}_engine_open_inner(out,caps,options,error,capacity);
    lmb_k3_source_release();pthread_mutex_unlock(&lmb_k3_open_lock);return rc;
}}

'''
        s=s[:at]+wrapper+s[at:]
    path.write_text(s)


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--copy',required=True,type=Path)
    prepare(p.parse_args().copy)
