/* Lumabri-owned lifetime for the pinned text-only V4.1 range engine.
 * Included after v41_range_core.c. Weights are immutable and owned by the
 * engine. A conversation owns ALL mutable state, including cache metadata.
 * DSpark and vision are intentionally absent from this contract.
 */
#ifndef LMB_V41_LIFECYCLE_H
#define LMB_V41_LIFECYCLE_H

#define LMB_V41_STATE_FIELDS(X) \
    X(window) X(window_pos) X(ckv) X(ikey) X(cstate_kv) X(cstate_score) \
    X(ring_save) X(ring_save_pos) X(cstate_save_kv) X(cstate_save_score) X(cstate_save_slot)
#define LMB_V41_W8_FIELDS(X) \
    X(wq_a) X(wq_b) X(wkv) X(wo_a) X(wo_b) X(idx_wq_b) \
    X(sh_w1) X(sh_w3) X(sh_w2) X(eng_wkv)
#define LMB_V41_WB_FIELDS(X) X(comp_wkv) X(comp_wgate) X(idx_wk) X(idx_wproj) X(gate_w)
#define LMB_V41_WF_FIELDS(X) \
    X(q_norm) X(kv_norm) X(attn_sink) X(attn_norm) X(ffn_norm) \
    X(hc_attn_fn) X(hc_ffn_fn) X(hc_attn_base) X(hc_ffn_base) \
    X(hc_attn_scale) X(hc_ffn_scale) X(comp_norm) X(idx_knorm) X(gate_bias) X(eng_q) X(eng_k)

static void lmb_v41_state_destroy(Model *m) {
    if (m->L) for (int i = 0; i < m->c.n_layers; i++) {
        Layer *l = &m->L[i];
#define DROP(p) free(l->p); l->p = NULL;
        LMB_V41_STATE_FIELDS(DROP)
#undef DROP
    }
    if (m->cache) for (int i = 0; i < m->c.n_layers; i++) free(m->cache[i].slot);
    free(m->cache); m->cache = NULL;
    if (m->ehit) for (int i = 0; i < m->c.n_layers; i++) free(m->ehit[i]);
    free(m->ehit); m->ehit = NULL;
    for (int i = 0; i < m->engram.n_layers; i++) {
        EngramTable *t = &m->engram.table[i];
        free(t->key); free(t->used); free(t->value);
        t->key = NULL; t->used = NULL; t->value = NULL;
    }
    free(m->engram.history); m->engram.history = NULL;
    free(m->candidates); m->candidates = NULL;
    free(m->shared_topk); m->shared_topk = NULL;
    free(m->pub_layer); m->pub_layer = NULL;
    free(m->main_hidden); m->main_hidden = NULL;
    kv_prefix_free(&m->kvp);
}

/* Drop weights only for their engine owner, never for a session view. */
static void lmb_v41_model_destroy(Model *m, int owns_weights) {
    if (owns_weights) {
        if (m->L) for (int i = 0; i < m->c.n_layers; i++) {
            Layer *l = &m->L[i];
#define DROP8(p) free(l->p.q); free(l->p.s);
#define DROPW(p) free(l->p.w);
            LMB_V41_W8_FIELDS(DROP8)
            LMB_V41_WB_FIELDS(DROPW)
            LMB_V41_WF_FIELDS(DROPW)
#undef DROP8
#undef DROPW
        }
        if (m->cache) for (int i = 0; i < m->c.n_layers; i++)
            for (int j = 0; j < m->cache[i].cap; j++)
                for (int k = 0; k < V41_EXPERT_TENSORS; k++) free(m->cache[i].slot[j].base[k]);
        for (int i = 0; i < m->engram.n_layers; i++) {
            free(m->engram.table[i].resident_w); free(m->engram.table[i].resident_s);
        }
        free(m->engram.token_map);
        free(m->embed.w); free(m->head.w); free(m->norm.w);
        free(m->rope_window); free(m->rope_compress);
        shards *s = &m->S;
        for (int i = 0; i < s->n; i++) free(s->t[i].name);
        for (int i = 0; i < s->nfd; i++) {
            if (s->fds[i] >= 0) close(s->fds[i]);
            if (s->dfds[i] >= 0) close(s->dfds[i]);
            for (int r = 0; r < s->nrep; r++) {
                if (s->mfds[r][i] >= 0) close(s->mfds[r][i]);
                if (s->mdfds[r][i] >= 0) close(s->mdfds[r][i]);
            }
            free(s->paths[i]);
        }
        for (int i = 0; i < s->fmt_n; i++) { free(s->fmt_name[i]); free(s->fmt_val[i]); }
        free(s->fmt_name); free(s->fmt_val); free(s->t); free(s->hidx);
    }
    lmb_v41_state_destroy(m);
    free(m->L);
    memset(m, 0, sizeof *m);
}

static void lmb_v41_state_reset(Model *m) {
    const Cfg *c = &m->c;
    for (int i = 0; i < c->n_layers; i++) {
        Layer *l = &m->L[i];
        if (l->window) memset(l->window, 0, (size_t)c->window * c->head_dim * sizeof(float));
        if (l->window_pos) for (int j = 0; j < c->window; j++) l->window_pos[j] = -1;
        int ratio = c->compress_ratio[i];
        if (c->kv_source[i]) {
            size_t slots = (size_t)c->max_positions / ratio;
            if (l->ckv) memset(l->ckv, 0, slots * c->head_dim * sizeof(float));
            if (l->ikey) memset(l->ikey, 0, slots * c->index_head_dim * sizeof(float));
            if (l->cstate_kv) memset(l->cstate_kv, 0, (size_t)ratio * c->head_dim * sizeof(float));
            if (l->cstate_score) for (int j = 0; j < ratio * c->head_dim; j++) l->cstate_score[j] = -INFINITY;
        }
    }
    for (int i = 0; i < m->engram.n_layers; i++) {
        EngramTable *t = &m->engram.table[i];
        for (int j = 0; j < t->cap; j++) { t->key[j] = -1; t->used[j] = 0; }
        t->clock = t->hits = t->misses = 0;
    }
    m->engram.history_len = 0;
    m->candidate_width = m->candidate_rows = 0;
    m->shared_topk_width = m->shared_topk_rows = 0;
    m->published_index_k = m->pub_before = NULL;
    m->published_index_layer = m->pub_before_layer = m->pub_rows = 0;
    m->pos = m->last_start = m->last_rows = m->main_hidden_rows = m->rollback_save = 0;
    m->boundary_input = m->boundary_mix = NULL;
    m->boundary_output = m->boundary_output_mix = NULL;
    kv_prefix_clear(&m->kvp);
}

/* Clone only a prepared engine. The metadata and weights remain borrowed.
 * Engine lifetime must cover every session, as enforced by the public ABI. */
static int lmb_v41_session_model(Model *out, const Model *engine) {
    *out = *engine;
    const Cfg *c = &engine->c;
    out->L = NULL; out->cache = NULL; out->ehit = NULL;
    out->candidates = NULL; out->shared_topk = NULL; out->pub_layer = NULL; out->main_hidden = NULL;
    out->engram.history = NULL; out->engram.history_cap = 0;
    memset(&out->kvp, 0, sizeof out->kvp);
    for (int i = 0; i < out->engram.n_layers; i++) {
        EngramTable *t = &out->engram.table[i];
        t->key = NULL; t->used = NULL; t->value = NULL;
    }
    out->L = calloc((size_t)c->n_layers, sizeof(Layer));
    if (!out->L) goto fail;
    for (int i = 0; i < c->n_layers; i++) {
        out->L[i] = engine->L[i];
#define ZERO(p) out->L[i].p = NULL;
        LMB_V41_STATE_FIELDS(ZERO)
#undef ZERO
    }
    out->cache = calloc((size_t)c->n_layers, sizeof(LCache));
    if (!out->cache) goto fail;
    for (int i = 0; i < c->n_layers; i++) {
        Layer *l = &out->L[i]; const Layer *src = &engine->L[i];
        int ratio = c->compress_ratio[i];
        size_t slots = ratio > 0 ? (size_t)c->max_positions / ratio : 0;
        size_t saved = c->spec_block > 0 ? (size_t)c->spec_block + 1 : 1;
#define ALLOC(p, n) do { if (src->p) { l->p = calloc((n), sizeof(*l->p)); if (!l->p) goto fail; } } while (0)
        ALLOC(window, (size_t)c->window * c->head_dim); ALLOC(window_pos, (size_t)c->window);
        ALLOC(ckv, slots * c->head_dim); ALLOC(ikey, slots * c->index_head_dim);
        ALLOC(cstate_kv, (size_t)ratio * c->head_dim); ALLOC(cstate_score, (size_t)ratio * c->head_dim);
        ALLOC(ring_save, saved * c->head_dim); ALLOC(ring_save_pos, saved);
        ALLOC(cstate_save_kv, saved * c->head_dim); ALLOC(cstate_save_score, saved * c->head_dim); ALLOC(cstate_save_slot, saved);
#undef ALLOC
        const LCache *src_cache = &engine->cache[i];
        if (src_cache->cap) {
            out->cache[i] = *src_cache;
            out->cache[i].slot = malloc((size_t)src_cache->cap * sizeof(Slot));
            if (!out->cache[i].slot) goto fail;
            memcpy(out->cache[i].slot, src_cache->slot, (size_t)src_cache->cap * sizeof(Slot));
        }
    }
    for (int i = 0; i < out->engram.n_layers; i++) {
        EngramTable *t = &out->engram.table[i];
        if (!t->cap) continue;
        t->key = malloc((size_t)t->cap * sizeof(*t->key));
        t->used = calloc((size_t)t->cap, sizeof(*t->used));
        t->value = malloc((size_t)t->cap * out->engram.head_dim * sizeof(*t->value));
        if (!t->key || !t->used || !t->value) goto fail;
    }
    if (!kv_prefix_alloc(&out->kvp, c->max_positions)) goto fail;
    lmb_v41_state_reset(out);
    return 0;
fail:
    lmb_v41_model_destroy(out, 0);
    return -1;
}

#endif
