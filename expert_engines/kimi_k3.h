/* expert_engines/kimi_k3.h — the Kimi K3 glue.
 *
 * The odd one out in a way that matters on the wire: K3's routed experts do
 * NOT work in hidden space. The layer projects down to a latent of
 * c->latent, routes and runs the experts there, then projects back up. So
 * the activation this peer receives and returns is `latent` wide, not
 * `hidden` — and lmbe_hidden() reports latent, which is what the manifest's
 * dimension check compares against. Getting that wrong would be caught, but
 * only as "mismatched manifest", so it is worth naming.
 *
 * Weights are native MXFP4 out of the HF shards, six blocks in one slot
 * (w1 packed+scales, w2, w3), read by the engine's own expert_read. The
 * routing weight is applied by the caller at accumulation time in the
 * engine too, so splitting it across the wire changes nothing.
 */
#ifndef LMBE_KIMI_K3_H
#define LMBE_KIMI_K3_H

#define main kimi_main_unused
#include "kimi_k3.c"
#undef main

typedef Slot LmbeSlot;

static Model lmbe_M;

static const char *lmbe_engine_name(void) { return "kimi_k3"; }

static void lmbe_open(const char *dir, int cap, int bits) {
    (void)cap; (void)bits;            /* MXFP4 comes as it is on disk */
    model_init(&lmbe_M, dir, 0);
}

static int lmbe_n_slots(void)   { return lmbe_M.c.n_layers; }
static int lmbe_n_experts(void) { return lmbe_M.c.n_experts; }
static int lmbe_hidden(void)    { return lmbe_M.c.latent; }   /* the wire width */
static int lmbe_inter(void)     { return lmbe_M.c.moe_inter; }

static int lmbe_routed(int slot) {
    return slot >= 0 && slot < lmbe_M.c.n_layers && lmbe_M.L[slot].sparse;
}

static void lmbe_slot_init(LmbeSlot *s) { memset(s, 0, sizeof *s); s->eid = -1; }

static void lmbe_slot_load(int slot, int eid, LmbeSlot *s) {
    expert_read(&lmbe_M, slot, eid, s);   /* allocates s->base on first use */
    s->eid = eid;
}

typedef struct { float *gate, *up; } LmbeScratch;

static void *lmbe_scratch_new(int nrows) {
    (void)nrows;                      /* K3's kernels are one row at a time */
    LmbeScratch *sc = (LmbeScratch *)calloc(1, sizeof *sc);
    sc->gate = falloc(lmbe_M.c.moe_inter);
    sc->up   = falloc(lmbe_M.c.moe_inter);
    return sc;
}

static void lmbe_scratch_free(void *p) {
    LmbeScratch *sc = (LmbeScratch *)p;
    free(sc->gate); free(sc->up); free(sc);
}

/* kimi_k3.c's expert_apply(), minus the two things that belong to the
 * chatter: the router weight and the accumulation. */
static void lmbe_apply(const LmbeSlot *ec, int slot, const float *z, float *out,
                       int nrows, void *p) {
    (void)slot;
    LmbeScratch *sc = (LmbeScratch *)p;
    Slot *s = (Slot *)ec;
    Cfg *c = &lmbe_M.c;
    uint8_t *w1p = s->buf, *w1s = w1p + lmbe_M.e_w1p, *w2p = w1s + lmbe_M.e_w1s,
            *w2s = w2p + lmbe_M.e_w2p, *w3p = w2s + lmbe_M.e_w2s,
            *w3s = w3p + lmbe_M.e_w1p;
    void (*mm)(float *, const float *, const uint8_t *, const uint8_t *, int, int, int)
        = g_k3_idot ? matmul_mxfp4_i8 : matmul_mxfp4;
    for (int r = 0; r < nrows; r++) {
        const float *zr = z + (int64_t)r * c->latent;
        mm(sc->gate, zr, w1p, w1s, 1, c->latent, c->moe_inter);
        mm(sc->up,   zr, w3p, w3s, 1, c->latent, c->moe_inter);
        for (int i = 0; i < c->moe_inter; i++)
            sc->gate[i] = situf_(sc->gate[i], sc->up[i], c->situ_b1, c->situ_b2);
        mm(out + (int64_t)r * c->latent, sc->gate, w2p, w2s, 1, c->moe_inter, c->latent);
    }
}

#endif
