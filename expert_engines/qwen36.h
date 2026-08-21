/* expert_engines/qwen36.h — the Qwen3.6 (qwen36) glue.
 *
 * qwen36 is a hybrid DeltaNet/Gated-Attention model, but every layer carries a
 * streaming MoE block and that is the only part a peer runs. Written in olmoe's
 * dialect, so this mirrors olmoe.h — with two qwen36 specifics that must match
 * or the returned expert diverges in the low bits:
 *
 *   - experts are group-scaled: matmul_qe() dispatches to matmul_q_gs when the
 *     global g_expert_gs (from qwen36_meta.json) is set. We set it at open, and
 *     apply through matmul_qe, exactly as moe() does.
 *   - experts ship int4 or int8 in one container; slot_ensure_int8() is what
 *     rematerialises int8 before the matmuls, so we call it here too.
 *
 * The routed expert math below is the body of qwen36.c's moe() inner loop
 * (the CPU `else` branch; the CUDA tier's qt_ready() is a stub returning 0 in a
 * build without COLI_CUDA). The routing weight, the shared expert and the
 * accumulation stay on the chatter — the model's semantics, not the peer's.
 */
#ifndef LMBE_QWEN36_H
#define LMBE_QWEN36_H

#define main qwen36_main_unused
#include "qwen36.c"
#undef main

typedef Slot LmbeSlot;

static Model lmbe_M;

static const char *lmbe_engine_name(void) { return "qwen36"; }
static int lmbe_effective_bits(int bits) { (void)bits; return 0; }

static void lmbe_open(const char *dir, int cap, int bits) {
    (void)cap; (void)bits;            /* qwen36 reads the container's own format */
    load_cfg(&lmbe_M.c, dir);
    int n_layers_from_config = lmbe_M.c.n_layers;
    load_meta(&lmbe_M.c, dir);
    validate_cfg(&lmbe_M.c, n_layers_from_config);
    st_init(&lmbe_M.S, dir);
    g_expert_gs = lmbe_M.c.expert_gs;   /* the engine sets this after load; match it */
    /* load_expert_merged() indexes m->active_of[layer]; the full model_init sets
     * it, which the peer skips, so build the same identity map here (Phase 2
     * stores every layer under its own index). Without this the resident-mode
     * preload dereferences a NULL and the node segfaults before serving. */
    lmbe_M.active_of = (int *)malloc((size_t)lmbe_M.c.n_layers * sizeof(int));
    if (!lmbe_M.active_of) { fprintf(stderr, "OOM allocating active_of\n"); exit(1); }
    for (int i = 0; i < lmbe_M.c.n_layers; i++) lmbe_M.active_of[i] = i;
}

static int lmbe_n_slots(void)   { return lmbe_M.c.n_layers; }
static int lmbe_n_experts(void) { return lmbe_M.c.n_experts; }
static int lmbe_hidden(void)    { return lmbe_M.c.hidden; }   /* experts work in hidden space */
static int lmbe_inter(void)     { return lmbe_M.c.inter; }
static int lmbe_routed(int slot) { (void)slot; return 1; }    /* every layer has a MoE block */

static void lmbe_slot_init(LmbeSlot *s) { slot_ensure_allocated(&lmbe_M, s); }

static void lmbe_slot_load(int slot, int eid, LmbeSlot *s) {
    load_expert_merged(&lmbe_M, slot, eid, s);
    s->eid = eid;
}

typedef struct { float *g, *u, *hh; } LmbeScratch;

static void *lmbe_scratch_new(int nrows) {
    (void)nrows;                      /* qwen36's expert kernel is one row at a time */
    LmbeScratch *sc = (LmbeScratch *)calloc(1, sizeof *sc);
    sc->g  = falloc(lmbe_M.c.inter);
    sc->u  = falloc(lmbe_M.c.inter);
    sc->hh = falloc(lmbe_M.c.hidden);
    return sc;
}

static void lmbe_scratch_free(void *p) {
    LmbeScratch *sc = (LmbeScratch *)p;
    free(sc->g); free(sc->u); free(sc->hh); free(sc);
}

/* Exactly qwen36.c's moe() routed inner loop, per row, unweighted: the down
 * projection of SwiGLU(gate(x), up(x)). slot_ensure_int8 first (int4 -> int8 if
 * needed), matmul_qe throughout (group scale via g_expert_gs). The chatter
 * multiplies by the routing weight and accumulates, and runs the shared expert
 * itself — identical to how the engine's moe() finishes the row. */
static void lmbe_apply(const LmbeSlot *e, int slot, const float *x, float *out,
                       int nrows, const float *w, void *p) {
    (void)w;                          /* this engine weights on the chatter */
    (void)slot;
    LmbeScratch *sc = (LmbeScratch *)p;
    Slot *es = (Slot *)e;             /* slot_ensure_int8 rematerialises in place */
    int D = lmbe_M.c.hidden, I = lmbe_M.c.inter;
    slot_ensure_int8(&lmbe_M, es);
    for (int r = 0; r < nrows; r++) {
        const float *xr = x + (int64_t)r * D;
        matmul_qe(sc->g, xr, es->g, es->gs, D, I);
        matmul_qe(sc->u, xr, es->u, es->us, D, I);
        for (int i = 0; i < I; i++) {
            float gv = sc->g[i];
            sc->g[i] = (gv / (1.f + expf(-gv))) * sc->u[i];
        }
        matmul_qe(out + (int64_t)r * D, sc->g, es->d, es->ds, I, D);
    }
}

#endif
