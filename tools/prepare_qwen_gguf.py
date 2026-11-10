"""Connect native GGUF to generated Qwen Edge/Segment adapters."""
from pathlib import Path
from prepare_qwen_variants import once


def wrap(s, prefix, kind):
    struct = 'Qwen36' + kind + 'Engine'
    start = s.index('static int ' + prefix + '_engine_open(')
    table = s.index('static const Coli' + kind + 'Adapter ', start)
    # The originals retain their own cleanup calls. Wrappers own the GGUF
    # header, so failed opens never double-close an unassigned source.
    region_start = s.index('static void ' + prefix + '_engine_destroy(') if kind == 'Edge' else start
    region = s[region_start:table]
    methods = ['engine_open', 'engine_destroy']
    if kind == 'Segment': methods += ['session_run']
    else: methods += ['select', 'logits']
    for method in methods:
        region = region.replace(prefix + '_' + method + '(', prefix + '_' + method + '_inner(')
    s = s[:region_start] + region + s[table:]
    table = s.index('static const Coli' + kind + 'Adapter ', region_start)
    code = f'''
static int {prefix}_engine_open(void **out, Coli{kind}Capabilities *caps,
    const Coli{kind}EngineOptions *options, char *error, size_t error_size) {{
    if (!out || !caps || !options || !options->model_dir) return -1;
    *out=NULL;
    LmbQwenSource source={{0}};
    if(lmb_qwen_source_open(&source,options->model_dir,error,error_size))return -1;
    LmbQwenSource previous=lmb_qwen_source_enter(&source);
    int rc={prefix}_engine_open_inner(out,caps,options,error,error_size);
    lmb_qwen_source_leave(&source,&previous);
    if(rc)gguf_close(&source.gg);
    else (({struct} *)*out)->source=source;
    return rc;
}}
static void {prefix}_engine_destroy(void *opaque) {{
    if(!opaque)return;
    {struct} *engine=opaque;
    LmbQwenSource source=engine->source, previous=lmb_qwen_source_enter(&source);
    {prefix}_engine_destroy_inner(opaque);
    lmb_qwen_source_leave(&source,&previous);
    gguf_close(&source.gg);
}}
'''
    if kind == 'Segment':
        code += f'''
static int {prefix}_session_run(void *opaque,const ColiSegmentRunRequest *req,char *err,size_t cap) {{
    if(!opaque)return -1;
    Qwen36SegmentEngine *e=((Qwen36SegmentSession *)opaque)->engine;
    LmbQwenSource previous=lmb_qwen_source_enter(&e->source);
    int rc={prefix}_session_run_inner(opaque,req,err,cap);
    lmb_qwen_source_leave(&e->source,&previous);return rc;
}}
'''
    else:
        for method in ['select', 'logits']:
            req = 'ColiEdge' + method.title() + 'Request'
            code += f'''
static int {prefix}_{method}(void *opaque,const {req} *req,char *err,size_t cap) {{
    if(!opaque)return -1;
    {struct} *e=opaque;
    LmbQwenSource previous=lmb_qwen_source_enter(&e->source);
    int rc={prefix}_{method}_inner(opaque,req,err,cap);
    lmb_qwen_source_leave(&e->source,&previous);return rc;
}}
'''
    return s[:table] + code + s[table:]


def prepare(root):
    path = Path(root) / 'qwen36.c'
    s = path.read_text()
    # A native GGUF must not inherit the first converted model's int4 mode.
    # This is per Model, not per process. The remaining switches describe
    # the process-wide CPU backend/environment, independent of checkpoint.
    s=once(s,'    GqExp *gqx; int64_t gqe_max;',
        '    int lmb_xf_set, lmb_xf_mode;\n    GqExp *gqx; int64_t gqe_max;')
    start=s.index('static int xf_mode(Model *m) {')
    end=s.index('\n/* The int4 experts',start)
    body=s[start:end]
    body=once(body,'    static int v = -1;\n    if (v >= 0) return v;',
        '    if (m->lmb_xf_set) return m->lmb_xf_mode;\n    int v;')
    body=once(body,'    v = on;', '    v = on; m->lmb_xf_set=1; m->lmb_xf_mode=v;')
    s=s[:start]+body+s[end:]
    start=s.index('static int xf_a32(Model *m) {')
    end=s.index('static int xf_run_mode(',start)
    s=s[:start]+'''static int xf_a32(Model *m) {
#ifdef XF16_ON
    const char *e=getenv("Q36_ILV16");
    return !(e && !strcmp(e,"0")) && xf_mode(m) && xf_act_mode()==1 &&
        q36_eh(&m->c)%16==0 && m->c.inter%16==0;
#else
    (void)m;return 0;
#endif
}
static int xf_r16(Model *m) {return xf_a32(m) && !(qt_ready() && qt_experts_on());}
'''+s[end:]
    s=once(s,'static int64_t g_pw_gp;', '/* Slot offsets follow the active Model, not the previous allocation. */\nstatic int64_t lmb_qwen_unused_pw;')
    s=s.replace('s->pw + g_pw_gp', 's->pw + (int64_t)m->c.inter*q36_eh(&m->c)/2')
    s=s.replace('s->pw + 2 * g_pw_gp', 's->pw + (int64_t)m->c.inter*q36_eh(&m->c)')
    s=s.replace('e->pw + g_pw_gp', 'e->pw + (int64_t)m->c.inter*q36_eh(&m->c)/2')
    s=s.replace('e->pw + 2 * g_pw_gp', 'e->pw + (int64_t)m->c.inter*q36_eh(&m->c)')
    s=once(s,'        g_pw_gp = ng / 2;', '        (void)ng;')
    s=s.replace('static int64_t lmb_qwen_unused_pw;', '')
    anchor = '#ifdef COLI_SEGMENT_ADAPTER\n/* ---------- engine-owned Segment adapter'
    s = once(s, anchor, '''#if defined(COLI_SEGMENT_ADAPTER) || defined(COLI_EDGE_ADAPTER)
#include "engine_patches/qwen_gguf_source.h"
#endif
'''+anchor)
    s = once(s, '    uint32_t layer_begin, layer_end, context_tokens;\n    pthread_mutex_t run_lock;',
        '    LmbQwenSource source;\n    uint32_t layer_begin, layer_end, context_tokens;\n    pthread_mutex_t run_lock;')
    s = once(s, '    Model model;\n} Qwen36EdgeEngine;', '    Model model;\n    LmbQwenSource source;\n} Qwen36EdgeEngine;')
    old = '''    load_cfg(&config, options->model_dir);
    int configured_layers = config.n_layers;
    load_meta(&config, options->model_dir);
    validate_cfg(&config, configured_layers);'''
    s = once(s, old, '''    if(g_gguf) {gguf_load_cfg(&config);validate_cfg(&config,config.n_layers);}
    else {
'''+old+'''
    }''')
    old = '''    load_cfg(&engine->model.c, options->model_dir);
    int config_layers = engine->model.c.n_layers;
    load_meta(&engine->model.c, options->model_dir);
    validate_cfg(&engine->model.c, config_layers);
    st_init(&engine->model.S, options->model_dir);'''
    s = once(s,old,'''    if(g_gguf) {gguf_load_cfg(&engine->model.c);validate_cfg(&engine->model.c,engine->model.c.n_layers);}
    else {
'''+old+'''
    }''')
    anchor = '    model_init_range(&engine->model, options->model_dir, cap, 8,'
    s = once(s,anchor,'''    /* Native slot sizes are validated by the GGUF planner and the locked
     * allocator, not by the old estimate for int8 converted experts. */
    if(g_gguf && getenv("LUMABRI_RESIDENT_REQUIRED") && !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"),"1"))
        cap=config.n_experts;
'''+anchor)
    anchor='engine->model.quant_bits = container_layer_is_int4(&engine->model, probe_layer) ? 4 : 8;'
    if s.count(anchor)!=2:raise RuntimeError('Qwen numeric class anchors changed')
    s=s.replace(anchor,'engine->model.quant_bits = g_gguf ? 0 : container_layer_is_int4(&engine->model, probe_layer) ? 4 : 8;')
    anchor='"qwen36/f32-int%d/cpu-v1", engine->model.quant_bits'
    if s.count(anchor)!=2:raise RuntimeError('Qwen capabilities anchors changed')
    s=s.replace(anchor,'g_gguf ? "qwen36/f32-gguf/cpu-v1" : "qwen36/f32-int%d/cpu-v1", engine->model.quant_bits')
    # Native expert maps own scale arrays even for layers outside this range.
    s=once(s,'    free(model->cache); free(model->active_of); free(model->L);', '''    if(model->gqx) {
        for(int i=0;i<model->c.n_layers;i++) {
            free(model->gqx[i].sg);free(model->gqx[i].su);free(model->gqx[i].sd);
        }
        free(model->gqx);
    }
    free(model->cache); free(model->active_of); free(model->L);''')
    # Muse transforms final logits after the head, just like step().
    s=once(s,'        if (coli_edge_argmax(logits, (uint32_t)config->vocab,',
           '        q36_logits_out(config,logits,config->vocab);\n        if (coli_edge_argmax(logits, (uint32_t)config->vocab,')
    anchor='''                 1, config->hidden, config->vocab);
    }
    free(normalized);'''
    s=once(s,anchor,'''                 1, config->hidden, config->vocab);
        q36_logits_out(config,request->logits+(size_t)row*config->vocab,config->vocab);
    }
    free(normalized);''')
    # Edge embedding expands to F32. The quantized native head is smaller,
    # but may be packed; account the actual held representation, not 2*F32.
    anchor='    uint64_t resident = (2u * cells + (uint64_t)config->hidden) * sizeof(float);'
    s=once(s,anchor,'''    const QW *head=&engine->model.lm_head;
    uint64_t head_bytes=head->gq ? (uint64_t)gq_row_bytes(head->gq_type,config->hidden)*config->vocab : cells*sizeof(float);
    uint64_t resident=(cells+(uint64_t)config->hidden)*sizeof(float)+head_bytes;''')
    s=once(s,'''        memcpy(output + (size_t)row * config->hidden,
               engine->model.embed + (size_t)token * config->hidden,
               (size_t)config->hidden * sizeof(float));''',
        '''        q36_embed_row(&engine->model,token,INT_MAX,output+(size_t)row*config->hidden);''')
    # Dense-only ranges have no expert_get iteration to check the budget.
    # The preparation acknowledgement must cover their weights as well.
    start=s.index('static int qwen36_segment_engine_open(')
    end=s.index('static void qwen36_segment_engine_destroy(',start)
    part=s[start:end]
    part=once(part,'        lmb_resident_adapter_prepared();', '''        if(lmb_resident_budget_exceeded(options->memory_limit_bytes)) {
            qwen36_segment_model_destroy(engine);
            pthread_mutex_destroy(&engine->run_lock);free(engine);
            return coli_segment_adapter_error(error,error_size,"resident dense preparation exceeded the approved memory budget");
        }
        lmb_resident_adapter_prepared();''')
    s=s[:start]+part+s[end:]
    s=wrap(s,'qwen36_segment','Segment')
    s=wrap(s,'qwen36_edge','Edge')
    path.write_text(s)
