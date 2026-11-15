"""Own and account for native Inkling GGUF weights in generated adapters."""
from pathlib import Path
from prepare_qwen_variants import once


def prepare(root):
    path=Path(root)/'inkling.c';s=path.read_text()
    a=s.index('static void load_cfg(');b=s.index('/* ---------- weight loading',a)
    s=s[:a]+once(s[a:b],'    free(buf); free(arena);','    json_free(root); free(buf); free(arena);')+s[b:]
    s=once(s,'    shards S;','    shards S;\n    int lmb_gguf;')
    for needle in ('    st_init(&m->S, snap);','    st_init(&model->S, options->model_dir);'):
        target='m' if '&m->' in needle else 'model'
        s=once(s,needle,needle+f'\n    {target}->lmb_gguf=st_gguf_active;')
    # These block buffers are owned allocations, independent of float/BF16 and
    # legacy packed matrices. Free them on both Edge and Segment shutdown.
    for kind in ('segment','edge'):
        a=s.index('static void inkling_'+kind+'_wt_destroy(');b=s.index('\n}',a)
        part=once(s[a:b],'    memset(weight, 0, sizeof(*weight));',
            '    free((void *)weight->gq);\n    memset(weight, 0, sizeof(*weight));')
        s=s[:a]+part+s[b:]
    s=once(s,'    free(model->cache); free(model->L);',
        '''    free(model->gx_t); free(model->gx_h);
    if(model->ehit) {
        for(int i=0;i<model->c.n_layers;i++)free(model->ehit[i]);
        free(model->ehit);
    }
    free(model->cache); free(model->L);''')
    s=once(s,'    if (weight->f) return rows * columns * sizeof(float);',
        '    if (weight->gq) return (uint64_t)weight->qn;\n    if (weight->f) return rows * columns * sizeof(float);')
    anchor='    InklingSegmentEngine *engine = calloc(1, sizeof(*engine));'
    s=once(s,anchor,'''    /* Fully resident GGUF slots use their native block sizes, not the float
     * estimate above. The planner reserves every slot; preparation checks the
     * actual process budget after each fill. No streaming fallback is allowed. */
    if(getenv("LUMABRI_RESIDENT_REQUIRED") && !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"),"1"))
        cap=config.n_experts;
'''+anchor)
    for kind in ('segment','edge'):
        a=s.index('static int inkling_'+kind+'_engine_open(');b=s.index('    *engine_impl = engine;',a)
        s=s[:a]+once(s[a:b],'"inkling/expert-q%d/f32/cpu-v1", bits);',
            'engine->model.lmb_gguf ? "inkling/gguf-q%d/f32/cpu-v1" : "inkling/expert-q%d/f32/cpu-v1", bits);')+s[b:]
    a=s.index('#ifdef COLI_SEGMENT_ADAPTER\n',s.index('#endif /* INKLING_NO_MAIN */'))
    s=s[:a]+'''#if defined(COLI_SEGMENT_ADAPTER) || defined(COLI_EDGE_ADAPTER)
static pthread_mutex_t lmb_ink_open_lock=PTHREAD_MUTEX_INITIALIZER;
static void lmb_ink_source_release(void) {
    gguf_close(&st_gguf_meta);gguf_close(&st_gguf_head);st_gguf_active=0;
}
#endif
'''+s[a:]
    for kind in ('segment','edge'):
        prefix='inkling_'+kind;title=kind.title()
        a=s.index('static int '+prefix+'_engine_open(')
        s=s[:a]+s[a:].replace(prefix+'_engine_open(',prefix+'_engine_open_inner(',1)
        at=s.index('static const Coli'+title+'Adapter '+prefix+'_adapter')
        wrapper=f'''static int {prefix}_engine_open(void **out,Coli{title}Capabilities *caps,
    const Coli{title}EngineOptions *options,char *error,size_t capacity) {{
    pthread_mutex_lock(&lmb_ink_open_lock);
    int rc={prefix}_engine_open_inner(out,caps,options,error,capacity);
    lmb_ink_source_release();pthread_mutex_unlock(&lmb_ink_open_lock);return rc;
}}

'''
        s=s[:at]+wrapper+s[at:]
    path.write_text(s)


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--copy',required=True,type=Path)
    prepare(p.parse_args().copy)
