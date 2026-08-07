/* expert_engines/inkling.h — the inkling glue.
 *
 * Dense-first layers like colibri (c->sparse[]), no MTP row. Three weight
 * layouts depending on what the container holds — a colibri-style packed
 * container (xq), runtime int quantization, or raw f32 — and the expert is
 * ONE fused gate+up matmul of 2*I rows followed by down, not two separate
 * ones.
 *
 * inkling computes its experts a row at a time locally, so unlike colibri
 * several rows here are several passes of the same loop: that IS the local
 * arithmetic, not an approximation of it.
 *
 * The slots are allocated the way slot_acquire() allocates them, one at a
 * time instead of carved from a per-layer slab: the engine's own LRU is not
 * used, because the peer has its own with refcounts a server needs and the
 * engine's does not have.
 */
#ifndef LMBE_INKLING_H
#define LMBE_INKLING_H

#define main inkling_main_unused
#include "inkling.c"
#undef main

typedef Slot LmbeSlot;

static Model lmbe_M;

static const char *lmbe_engine_name(void) { return "inkling"; }

static void lmbe_open(const char *dir, int cap, int bits) {
    (void)cap;
    setenv("PIN", "off", 0);          /* no cache warming: we own residency */
    model_init(&lmbe_M, dir, 1, bits);
}

static int lmbe_n_slots(void)   { return lmbe_M.c.n_layers; }
static int lmbe_n_experts(void) { return lmbe_M.c.n_experts; }
static int lmbe_hidden(void)    { return lmbe_M.c.hidden; }
static int lmbe_inter(void)     { return lmbe_M.c.moe_inter; }

static int lmbe_routed(int slot) {
    return slot >= 0 && slot < lmbe_M.c.n_layers && lmbe_M.c.sparse[slot];
}

/* slot_acquire()'s allocation, minus the LRU */
static void lmbe_slot_init(LmbeSlot *s) {
    Cfg *c = &lmbe_M.c;
    int64_t D = c->hidden, I = c->moe_inter, n13 = 2*I*D, n2 = D*I;
    memset(s, 0, sizeof *s);
    s->eid = -1;
    if (lmbe_M.xq) {
        int64_t st13 = lmbe_M.rb13*2*I, st2 = lmbe_M.rb2*D;
        void *w = NULL, *sc = NULL;
        if (posix_memalign(&w, 16384, (size_t)(st13+st2)) ||
            posix_memalign(&sc, 16384, (size_t)(2*I+D)*sizeof(float))) {
            fprintf(stderr, "OOM expert slot\n"); exit(1);
        }
        s->p13 = (uint8_t *)w; s->p2 = s->p13 + st13;
        s->s13 = (float *)sc;  s->s2 = s->s13 + 2*I;
    } else if (lmbe_M.quant_bits) {
        s->q13 = malloc((size_t)n13); s->q2 = malloc((size_t)n2);
        s->s13 = falloc(2*I);         s->s2 = falloc(D);
        if (!s->q13 || !s->q2) { fprintf(stderr, "OOM expert slot\n"); exit(1); }
    } else {
        s->f13 = falloc(n13); s->f2 = falloc(n2);
    }
}

static void lmbe_slot_load(int slot, int eid, LmbeSlot *s) {
    s->eid = eid;
    slot_fill(&lmbe_M, slot, s);
}

typedef struct { float *g, *hh; } LmbeScratch;

static void *lmbe_scratch_new(int nrows) {
    (void)nrows;                      /* inkling's kernels are one row at a time */
    LmbeScratch *sc = (LmbeScratch *)calloc(1, sizeof *sc);
    sc->g  = falloc(2 * lmbe_M.c.moe_inter);   /* gate rows then up rows */
    sc->hh = falloc(lmbe_M.c.hidden);
    return sc;
}

static void lmbe_scratch_free(void *p) {
    LmbeScratch *sc = (LmbeScratch *)p;
    free(sc->g); free(sc->hh); free(sc);
}

/* inkling.c's moe() compute branch, verbatim, once per row. */
static void lmbe_apply(const LmbeSlot *ec, int slot, const float *x, float *out,
                       int nrows, const float *w, void *p) {
    (void)w;                          /* this engine weights on the chatter */
    (void)slot;
    LmbeScratch *sc = (LmbeScratch *)p;
    Slot *e = (Slot *)ec;
    Cfg *c = &lmbe_M.c;
    int D = c->hidden, I = c->moe_inter;
    int q4 = lmbe_M.xq && lmbe_M.rb13*2 == D;   /* packed int4 vs int8 container */
    float *g = sc->g, *u = g + I;
    for (int r = 0; r < nrows; r++) {
        const float *xs = x + (int64_t)r * D;
        float *hh = out + (int64_t)r * D;
        if (lmbe_M.xq) {
            if (q4) {
                matmul_q4(g, xs, e->p13, e->s13, D, 2*I);
                for (int i = 0; i < I; i++) g[i] = siluf(g[i]) * u[i];
                matmul_q4(hh, g, e->p2, e->s2, I, D);
            } else {
                matmul_q(g, xs, (int8_t *)e->p13, e->s13, D, 2*I);
                for (int i = 0; i < I; i++) g[i] = siluf(g[i]) * u[i];
                matmul_q(hh, g, (int8_t *)e->p2, e->s2, I, D);
            }
        } else if (lmbe_M.quant_bits) {
            matmul_q(g, xs, e->q13, e->s13, D, 2*I);
            for (int i = 0; i < I; i++) g[i] = siluf(g[i]) * u[i];
            matmul_q(hh, g, e->q2, e->s2, I, D);
        } else {
            matmul(g, xs, e->f13, 1, D, 2*I);
            for (int i = 0; i < I; i++) g[i] = siluf(g[i]) * u[i];
            matmul(hh, g, e->f2, 1, I, D);
        }
    }
}

#endif
