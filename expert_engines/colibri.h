/* expert_engines/colibri.h — the glue for the generic colibri engine (GLM).
 *
 * Three things make this shape different from olmoe's, and all three are
 * load-bearing:
 *
 *   dense layers   `first_k_dense_replace` layers have no experts at all.
 *                  Announcing experts for them would make every chatter
 *                  think the swarm is incomplete.
 *   the MTP slot   layer index n_layers is the multi-token-prediction layer,
 *                  and it routes to its own experts. The engine loads them
 *                  with the very same expert_load(layer == n_layers), so the
 *                  slot count here is n_layers + 1.
 *   fmt 6 (E8)     those experts store W@Q, so the gate/up input must be
 *                  rotated by Q^T first and the down input rotated again —
 *                  per expert, not per layer, which is why the rotation is
 *                  here and not on the chatter.
 *
 * The math below is colibri.c's own single-expert CPU path with nr=1.
 */
#ifndef LMBE_COLIBRI_H
#define LMBE_COLIBRI_H

#define main colibri_main_unused
#include "colibri.c"
#undef main

typedef ESlot LmbeSlot;

static Model lmbe_M;
static int lmbe_slots;

static const char *lmbe_engine_name(void) { return "colibri"; }

/* `bits` is the engine's expert quantization and it MUST match the chatter's,
 * because for a model without pre-quantized .qs tensors the loader quantizes
 * on the way in: a peer at 0 bits and an engine at 8 hold different weights
 * and the tokens diverge — quietly, a few positions in. It is the engine's
 * argv[2], default 8, and `expert_node --bits` carries it here.
 *
 * cap is the engine's own resident-expert budget. A peer keeps its own LRU
 * (or holds everything), so the engine's cache would only duplicate it: ask
 * for the smallest legal one and let PIN=0 keep model_init from warming it. */
static void lmbe_open(const char *dir, int cap, int bits) {
    (void)cap;
    setenv("PIN", "0", 0);            /* no autopin: we own residency */
    model_init(&lmbe_M, dir, 1, bits, bits);
    lmbe_slots = lmbe_M.c.n_layers + 1;
}

static int lmbe_n_slots(void)   { return lmbe_slots; }
static int lmbe_n_experts(void) { return lmbe_M.c.n_experts; }
static int lmbe_hidden(void)    { return lmbe_M.c.hidden; }
static int lmbe_inter(void)     { return lmbe_M.c.moe_inter; }

/* the engine's own rule, verbatim (colibri.c, the tier-fill loop) */
static int lmbe_routed(int slot) {
    int nl = lmbe_M.c.n_layers;
    if (slot < 0 || slot > nl) return 0;
    return (slot < nl && lmbe_M.L[slot].sparse) || (slot == nl && lmbe_M.has_mtp);
}

static void lmbe_slot_init(LmbeSlot *s) { memset(s, 0, sizeof *s); s->eid = -1; }

static void lmbe_slot_load(int slot, int eid, LmbeSlot *s) {
    expert_load(&lmbe_M, slot, eid, s, 1, 1);
    s->eid = eid;
}

typedef struct { float *g, *u, *xr; int rows; } LmbeScratch;

static void *lmbe_scratch_new(int nrows) {
    LmbeScratch *sc = (LmbeScratch *)calloc(1, sizeof *sc);
    sc->rows = nrows < 1 ? 1 : nrows;
    sc->g  = falloc((int64_t)sc->rows * lmbe_M.c.moe_inter);
    sc->u  = falloc((int64_t)sc->rows * lmbe_M.c.moe_inter);
    sc->xr = falloc((int64_t)sc->rows * lmbe_M.c.hidden);
    return sc;
}

static void lmbe_scratch_free(void *p) {
    LmbeScratch *sc = (LmbeScratch *)p;
    free(sc->g); free(sc->u); free(sc->xr); free(sc);
}

/* colibri.c's single-expert CPU path, verbatim, with the caller's row count.
 * nr rows in one call is NOT the same arithmetic as nr calls of one row, so
 * the row count has to come across the wire — see the protocol note in
 * lumabri_client.h. */
static void lmbe_apply(const LmbeSlot *ec, int slot, const float *x, float *out,
                       int nrows, void *p) {
    (void)slot;
    LmbeScratch *sc = (LmbeScratch *)p;
    ESlot *e = (ESlot *)ec;                 /* the engine's kernels take non-const */
    int D = lmbe_M.c.hidden, I = lmbe_M.c.moe_inter;
    const float *xs = x;
    if (e->g.fmt == 6) {                    /* E8: gate/up read x under Q^T */
        memcpy(sc->xr, x, (size_t)nrows * D * sizeof(float));
        e8_rot_rows(sc->xr, nrows, D);
        xs = sc->xr;
    }
    expert_gate_up(sc->g, sc->u, xs, &e->g, &e->u, nrows);
    for (int64_t z = 0; z < (int64_t)nrows * I; z++) sc->g[z] = siluf(sc->g[z]) * sc->u[z];
    if (e->d.fmt == 6) e8_rot_rows(sc->g, nrows, I);   /* down input, per expert */
    matmul_qt(out, sc->g, &e->d, nrows);
}

#endif
