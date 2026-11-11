"""Keep GLM5-Next GGUF metadata local to serialized adapter opens."""
from pathlib import Path
from prepare_qwen_variants import once


def prepare(root):
    path=Path(root)/'glm53.c';s=path.read_text()
    s=once(s,'    int has_io;', '    int lmb_gguf;\n    int has_io;')
    a=s.index('static void model_load_range(GModel *m, const char *dir, int layer_begin,')
    s=s[:a]+once(s[a:],'    st_init(&m->S, dir);',
        '    st_init(&m->S, dir);\n    m->lmb_gguf=st_gguf_active;')
    s=s.replace('"glm53/q%d-i4g64-experts/f32/cpu-v1", glm53_dense_bits()',
        'engine->model.lmb_gguf ? "glm53/q%d-gguf/f32/cpu-v1" : "glm53/q%d-i4g64-experts/f32/cpu-v1", glm53_dense_bits()')
    anchor='#ifdef COLI_SEGMENT_ADAPTER\n#include <pthread.h>'
    s=once(s,anchor,'''#if defined(COLI_SEGMENT_ADAPTER) || defined(COLI_EDGE_ADAPTER)
static pthread_mutex_t lmb_g53_open_lock=PTHREAD_MUTEX_INITIALIZER;
static void lmb_g53_source_release(void) {
    gguf_close(&st_gguf_meta);gguf_close(&st_gguf_head);st_gguf_active=0;
}
#endif
'''+anchor)
    for kind in ('segment','edge'):
        prefix='glm53_'+kind;title=kind.title()
        a=s.index('static int '+prefix+'_engine_open(')
        s=s[:a]+s[a:].replace(prefix+'_engine_open(',prefix+'_engine_open_inner(',1)
        at=s.index('static const Coli'+title+'Adapter '+prefix+'_adapter')
        wrapper=f'''static int {prefix}_engine_open(void **out,Coli{title}Capabilities *caps,
    const Coli{title}EngineOptions *options,char *error,size_t capacity) {{
    pthread_mutex_lock(&lmb_g53_open_lock);
    int rc={prefix}_engine_open_inner(out,caps,options,error,capacity);
    lmb_g53_source_release();pthread_mutex_unlock(&lmb_g53_open_lock);return rc;
}}

'''
        s=s[:at]+wrapper+s[at:]
    path.write_text(s)


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--copy',required=True,type=Path)
    prepare(p.parse_args().copy)
