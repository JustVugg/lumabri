/* expert_engines/olmoe.h — the olmoe glue.
 *
 * Uniform shape: every layer routes, no shared expert, no MTP slot. The
 * expert is three quantized matrices merged by the engine's own loader, and
 * the math below is the body of olmoe.c's moe() inner loop with S=1.
 */
#ifndef LMBE_OLMOE_H
#define LMBE_OLMOE_H

#define main olmoe_main_unused
#include "olmoe.c"
#undef main

typedef Slot LmbeSlot;

static Model lmbe_M;

static const char *lmbe_engine_name(void) { return "olmoe"; }
static int lmbe_effective_bits(int bits) { (void)bits; return 0; }

static void lmbe_open(const char *dir, int cap, int bits) {
    (void)cap; (void)bits;            /* olmoe reads the container's own format */
    load_cfg(&lmbe_M.c, dir);
    st_init(&lmbe_M.S, dir);
}

static int lmbe_n_slots(void)   { return lmbe_M.c.n_layers; }
static int lmbe_n_experts(void) { return lmbe_M.c.n_experts; }
static int lmbe_hidden(void)    { return lmbe_M.c.hidden; }
static int lmbe_inter(void)     { return lmbe_M.c.inter; }
static int lmbe_routed(int slot) { (void)slot; return 1; }

static void lmbe_slot_init(LmbeSlot *s) { slot_ensure_allocated(&lmbe_M, s); }

static void lmbe_slot_load(int slot, int eid, LmbeSlot *s) {
    load_expert_merged(&lmbe_M, slot, eid, s);
    s->eid = eid;
}

typedef struct { float *g, *u; } LmbeScratch;

static void *lmbe_scratch_new(int nrows) {
    (void)nrows;                      /* olmoe's kernels are one row at a time */
    LmbeScratch *sc = (LmbeScratch *)calloc(1, sizeof *sc);
    sc->g = falloc(lmbe_M.c.inter);
    sc->u = falloc(lmbe_M.c.inter);
    return sc;
}

static void lmbe_scratch_free(void *p) {
    LmbeScratch *sc = (LmbeScratch *)p;
    free(sc->g); free(sc->u); free(sc);
}

/* Exactly olmoe.c's moe() inner loop. That loop is per row, so several rows
 * are several passes of it — unlike the batching engines, here row-at-a-time
 * IS the local arithmetic. The routing weight and the accumulation stay with
 * the chatter: those are the model's semantics, not the peer's business. */
static void lmbe_apply(const LmbeSlot *e, int slot, const float *x, float *out,
                       int nrows, const float *w, void *p) {
    (void)w;                          /* this engine weights on the chatter */
    (void)slot;
    LmbeScratch *sc = (LmbeScratch *)p;
    int D = lmbe_M.c.hidden, I = lmbe_M.c.inter;
    for (int r = 0; r < nrows; r++) {
        const float *xr = x + (int64_t)r * D;
        matmul_q(sc->g, xr, e->g, e->gs, D, I);
        matmul_q(sc->u, xr, e->u, e->us, D, I);
        for (int i = 0; i < I; i++) {
            float gv = sc->g[i];
            sc->g[i] = (gv / (1.f + expf(-gv))) * sc->u[i];
        }
        matmul_q(out + (int64_t)r * D, sc->g, e->d, e->ds, I, D);
    }
}

#endif
