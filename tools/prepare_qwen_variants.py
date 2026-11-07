"""Extend only generated Colibri copies for heterogeneous qwen36 layers."""
from pathlib import Path


def once(s, old, new):
    if s.count(old) != 1:
        raise RuntimeError("qwen36 architecture anchor changed: " + old[:100])
    return s.replace(old, new, 1)


def prepare(root):
    path = Path(root) / 'qwen36.c'
    s = path.read_text()
    # Dense layers have no routed expert tensors. Do not prefetch nonexistent
    # experts at READY, even when this model has MoE in other layers.
    s = once(s, '''        for (uint32_t layer = engine->layer_begin; layer < engine->layer_end; layer++) {
            if (engine->model.cache[layer].cap < engine->model.c.n_experts) {''', '''        for (uint32_t layer = engine->layer_begin; layer < engine->layer_end; layer++) {
            if (!engine->model.c.n_experts || q36_dense_l(&engine->model.c, (int)layer)) continue;
            if (engine->model.cache[layer].cap < engine->model.c.n_experts) {''')
    # A partial interval can start on a dense Flint layer. The representation
    # belongs to the checkpoint, not to whichever tensor the range starts at.
    s = once(s, '''    engine->model.quant_bits = container_layer_is_int4(
        &engine->model, (int)options->layer_begin) ? 4 : 8;''', '''    int probe_layer = 0;
    while (probe_layer + 1 < engine->model.c.n_layers &&
           q36_dense_l(&engine->model.c, probe_layer)) probe_layer++;
    engine->model.quant_bits = container_layer_is_int4(&engine->model, probe_layer) ? 4 : 8;''')
    # Release all state owned by the newer layer types, including error paths.
    s = once(s, '''    free(layer->dn_norm); qw_free(&layer->dn_out);
}''', '''    free(layer->dn_norm); qw_free(&layer->dn_out);
    free(layer->sinks); free(layer->post_op); free(layer->post_ffn);
    qw_free(&layer->sc_in); free(layer->sc_conv); qw_free(&layer->sc_out);
}''')
    s = once(s, '''    free(model->c.is_attn);
    st_destroy(&model->S);''', '''    free(model->c.is_attn); free(model->c.win_layer);
    free(model->c.nope_layer); free(model->c.dense_layer);
    st_destroy(&model->S);''')
    s = once(s, '''    free(engine->model.c.is_attn);
    st_destroy(&engine->model.S);''', '''    free(engine->model.c.is_attn); free(engine->model.c.win_layer);
    free(engine->model.c.nope_layer); free(engine->model.c.dense_layer);
    st_destroy(&engine->model.S);''')
    # The preliminary config owns its masks too, independently of Model.c.
    s = s.replace('free(config.is_attn);', 'free(config.is_attn); free(config.win_layer); free(config.nope_layer); free(config.dense_layer);')
    # LFM3 has a short convolution without DeltaNet recurrence. Match the
    # monolithic allocator's one-cell placeholder on platforms where calloc(0)
    # may return NULL; the convolution ring still belongs to each session.
    s = once(s, 'session->DN_rec[layer] = calloc(cells, sizeof(float));',
             'session->DN_rec[layer] = calloc(cells ? cells : 1, sizeof(float));')
    s = once(s, 'm->cache[i].slot_by_expert = malloc((size_t)c->n_experts * sizeof(int));',
             'm->cache[i].slot_by_expert = malloc((size_t)(c->n_experts ? c->n_experts : 1) * sizeof(int));')
    # Cross-process source reads are forbidden after preparation. Signal Edge
    # completion separately; its embedding/head are loaded eagerly too.
    start = s.index('static int qwen36_edge_engine_open(')
    anchor = '    *engine_impl = engine;\n    return 0;'
    end = s.index(anchor, start)
    s = s[:end] + s[end:].replace(anchor, '''    if (getenv("LUMABRI_RESIDENT_REQUIRED") && !strcmp(getenv("LUMABRI_RESIDENT_REQUIRED"), "1")) {
        if (lmb_resident_budget_exceeded(options->memory_limit_bytes)) {
            qwen36_edge_engine_destroy(engine);
            return coli_edge_adapter_error(error,error_size,"resident Edge preparation exceeded the approved memory budget");
        }
        lmb_resident_adapter_prepared();
    }
'''+anchor, 1)
    path.write_text(s)
