/* Included only in Lumabri's disposable Qwen3.8 Edge build. Colibri's MTP
 * arithmetic (q38_mtp_feed, q38_mtp_draft, q38_mtp_draft_more) and the
 * upstream checkout are unchanged.
 *
 * Upstream attaches the head only to a process that holds the whole model and
 * keeps the head's state in the one Model. A household Edge holds no layer
 * and serves several conversations, so this file
 *   - loads the head alone into the Edge's Model: its layer (index
 *     c.layers), all of its routed experts, and its input and output
 *     projections, every byte resident before the Edge reports ready;
 *   - keeps the head's attention rows and pending rows per conversation and
 *     lends them to the Model around each call, as the Segment adapters do.
 * tools/prepare_qwen38_mtp.py checks that the upstream loader this mirrors
 * has not changed.
 */
#include "src/runtime/lumabri_draft_head.h"

typedef struct {
    float *K, *V, *IK;                /* the head's attention and indexer rows */
    float *pend;                      /* Q38_SPEC_ROWS rows of streams awaiting their next token */
    int pend_tok[Q38_SPEC_ROWS];
    int len, pend_n;
    uint32_t context;
} LmbQ38HeadSession;

static pthread_mutex_t lmb_q38_head_lock = PTHREAD_MUTEX_INITIALIZER;

/* 1: this checkpoint has a head this engine runs. */
static int lmb_q38_head_present(Model *m) {
    char eh[96];
    if (!m->lmb_gguf || m->c.mtp_layers != 1) return 0;
    snprintf(eh, sizeof eh, "blk.%d.nextn.eh_proj.weight", m->c.layers);
    return gguf_find(&m->lmb_gg, eh) != NULL;
}

static void lmb_q38_head_close(Model *m) {
    if (!m->mtp) return;
    lmb_draft_head_unregister(m);
    Cfg *c = &m->c; int i = c->layers;
    if (m->L) q38_layer_free(&m->L[i]);
    if (m->cache) {
        for (int s = 0; m->cache[i].slots && s < m->cache[i].n; s++) {
            q38_weight_free(&m->cache[i].slots[s].gate);
            q38_weight_free(&m->cache[i].slots[s].up);
            q38_weight_free(&m->cache[i].slots[s].down);
            free(m->cache[i].slots[s].fp8_slab);
            free(m->cache[i].slots[s].int4_slab);
        }
        free(m->cache[i].slots); free(m->cache[i].by_expert);
    }
    if (m->expert_scales) free(m->expert_scales[i].values);
    /* The route trace has one row per model layer, none for the head. */
    if (m->ehit) { for (int r = 0; r < i; r++) free(m->ehit[r]); free(m->ehit); m->ehit = NULL; }
    free(m->L); free(m->cache); free(m->expert_scales); free(m->K); free(m->V); free(m->IK);
    m->L = NULL; m->cache = NULL; m->expert_scales = NULL; m->K = m->V = m->IK = NULL;
    free(m->mtp_norm_emb); free(m->mtp_norm_hid);
    q38_weight_free(&m->mtp_fc_emb); q38_weight_free(&m->mtp_fc_hid);
    free(m->mtp_mixer.norm); q38_weight_free(&m->mtp_mixer.down); q38_weight_free(&m->mtp_mixer.up);
    m->mtp_norm_emb = m->mtp_norm_hid = NULL; m->mtp_mixer.norm = NULL;
    m->mtp_pend = NULL; m->mtp = 0;
}

static void lmb_q38_head_lend(Model *m, LmbQ38HeadSession *s) {
    int i = m->c.layers;
    m->K[i] = s->K; m->V[i] = s->V; m->IK[i] = s->IK;
    m->kv_cap = m->max_t = (int)s->context;
    m->mtp_pend = s->pend; m->mtp_len = s->len; m->mtp_pend_n = s->pend_n;
    memcpy(m->mtp_pend_tok, s->pend_tok, sizeof s->pend_tok);
}
static void lmb_q38_head_return(Model *m, LmbQ38HeadSession *s) {
    int i = m->c.layers;
    s->len = m->mtp_len; s->pend_n = m->mtp_pend_n;
    memcpy(s->pend_tok, m->mtp_pend_tok, sizeof s->pend_tok);
    m->K[i] = m->V[i] = m->IK[i] = NULL; m->mtp_pend = NULL;
    m->mtp_len = m->mtp_pend_n = 0; m->kv_cap = m->max_t = 0;
}

static void lmb_q38_head_session_destroy(void *owner, void *session) {
    (void)owner;
    LmbQ38HeadSession *s = (LmbQ38HeadSession *)session;
    if (!s) return;
    free(s->K); free(s->V); free(s->IK); free(s->pend); free(s);
}

static int lmb_q38_head_session_create(void *owner, uint32_t context_tokens, void **session) {
    Model *m = (Model *)owner; Cfg *c = &m->c;
    if (!session) return -1;
    *session = NULL;
    if (!context_tokens || context_tokens > (uint32_t)c->max_positions) return -1;
    LmbQ38HeadSession *s = (LmbQ38HeadSession *)calloc(1, sizeof *s);
    if (!s) return -1;
    size_t kv = (size_t)c->kv_heads * context_tokens * (size_t)c->head_dim;
    s->context = context_tokens;
    s->K = (float *)calloc(kv, sizeof(float)); s->V = (float *)calloc(kv, sizeof(float));
    s->IK = (float *)calloc((size_t)context_tokens * (size_t)c->idx_dim, sizeof(float));
    s->pend = (float *)calloc((size_t)Q38_SPEC_ROWS * (size_t)c->hc_width, sizeof(float));
    if (!s->K || !s->V || !s->IK || !s->pend) { lmb_q38_head_session_destroy(owner, s); return -1; }
    *session = s;
    return 0;
}

static int lmb_q38_head_commit(void *owner, void *session, uint64_t position, uint32_t rows,
                               const int32_t *tokens, const float *streams) {
    Model *m = (Model *)owner; LmbQ38HeadSession *s = (LmbQ38HeadSession *)session;
    if (!s || !rows || !tokens || !streams || position > s->context || rows > s->context - position)
        return -1;
    for (uint32_t i = 0; i < rows; i++)
        if (tokens[i] < 0 || tokens[i] >= m->c.vocab) return -1;
    if (!position) { s->len = 0; s->pend_n = 0; }
    pthread_mutex_lock(&lmb_q38_head_lock);
    lmb_q38_head_lend(m, s);
    q38_mtp_feed(m, (const int *)tokens, (int)rows, (int)position, streams);
    lmb_q38_head_return(m, s);
    pthread_mutex_unlock(&lmb_q38_head_lock);
    return 0;
}

static uint32_t lmb_q38_head_propose(void *owner, void *session, int32_t token, uint64_t position,
                                     int32_t *out, uint32_t want) {
    Model *m = (Model *)owner; LmbQ38HeadSession *s = (LmbQ38HeadSession *)session;
    if (!s || !out || !want || token < 0 || token >= m->c.vocab || position >= s->context) return 0;
    if (want > LMB_DRAFT_HEAD_MAX) want = LMB_DRAFT_HEAD_MAX;
    /* A deeper draft writes a tail row at position + depth - 1. */
    if (want > s->context - position) want = (uint32_t)(s->context - position);
    float *hidden = (float *)malloc((size_t)m->c.hc_width * sizeof(float));
    if (!hidden) return 0;
    uint32_t made = 0;
    pthread_mutex_lock(&lmb_q38_head_lock);
    lmb_q38_head_lend(m, s);
    int draft = q38_mtp_draft(m, token, (int)position, hidden);
    if (draft >= 0) {
        out[made++] = draft;
        while (made < want) {
            draft = q38_mtp_draft_more(m, hidden, out[made - 1], (int)position + (int)made - 1);
            out[made++] = draft;
        }
    }
    lmb_q38_head_return(m, s);
    pthread_mutex_unlock(&lmb_q38_head_lock);
    free(hidden);
    return made;
}

/* 0: no head in this checkpoint, or the head is loaded, resident and
 * registered. -1: the checkpoint has a head that could not be prepared. */
static int lmb_q38_head_open(Model *m, uint64_t memory_limit_bytes) {
    if (!lmb_q38_head_present(m)) return 0;
    Cfg *c = &m->c; int H = c->hidden, W = c->hc_width, i = c->layers;
    _Static_assert(sizeof(int) == sizeof(int32_t), "token ids are int");
    m->L = (Layer *)calloc((size_t)c->layers + 1, sizeof(Layer));
    m->cache = (LCache *)calloc((size_t)c->layers + 1, sizeof(LCache));
    m->expert_scales = (Q38ExpertScaleCache *)calloc((size_t)c->layers + 1, sizeof(*m->expert_scales));
    m->K = (float **)calloc((size_t)c->layers + 1, sizeof(float *));
    m->V = (float **)calloc((size_t)c->layers + 1, sizeof(float *));
    m->IK = (float **)calloc((size_t)c->layers + 1, sizeof(float *));
    if (!m->L || !m->cache || !m->expert_scales || !m->K || !m->V || !m->IK) return -1;
    m->expert_prefetch = 0;
    m->expert_parallel_reads = q38_env_bool("Q38_EXPERT_PARALLEL_READS", 1);
    m->prefill_batch = q38_env_bool("Q38_PREFILL_BATCH", 1);
    m->mtp_wiring = Q38_MTP_STREAM_NORM;
    m->lmb_mtp_layer = i;
    snprintf(m->mtp_prefix, sizeof m->mtp_prefix, "mtp");
    /* LMB-MTP-LOADER-BEGIN: the tensors q38_mtp_attach loads, in its order */
    m->mtp_norm_emb = q38_load_tensor(m, "mtp.pre_fc_norm_embedding.weight", H);
    m->mtp_norm_hid = q38_load_tensor(m, "mtp.pre_fc_norm_hidden.weight", W);
    m->mtp_fc_emb = q38_load_weight(m, "mtp.fc_embedding.weight", H, H);
    m->mtp_fc_hid = q38_load_weight(m, "mtp.fc_hidden.weight", H, H);
    q38_load_gr_at(m, &m->mtp_mixer, "mtp.hyper_connection_mixer", 0);
    m->mtp = 1;
    m->mtp_drafts = 2;
    q38_load_layer(m, i, c->experts);
    /* LMB-MTP-LOADER-END */
    /* Every routed expert of the head is loaded now: nothing is read from
     * disk while a reply is generated. Under the resident contract the load
     * must also stay inside the Edge's approved memory. */
    const char *required = getenv("LUMABRI_RESIDENT_REQUIRED");
    int resident = required && !strcmp(required, "1");
    for (int eid = 0; eid < c->experts; eid++) {
        (void)q38_expert_get(m, i, eid);
        if (resident && lmb_resident_budget_exceeded(memory_limit_bytes)) return -1;
    }
    LmbDraftHeadBinding binding = {
        .owner = m,
        .session_create = lmb_q38_head_session_create,
        .session_destroy = lmb_q38_head_session_destroy,
        .commit = lmb_q38_head_commit,
        .propose = lmb_q38_head_propose,
    };
    if (lmb_draft_head_register(&binding)) return -1;
    fprintf(stderr, "[qwen38] MTP draft head resident at the Edge: layer %d, %d experts\n", i, c->experts);
    return 0;
}
