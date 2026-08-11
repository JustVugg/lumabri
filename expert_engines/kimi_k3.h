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
static int lmbe_effective_bits(int bits) { (void)bits; return 0; }

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

typedef struct { float *gate, *up, *hz; } LmbeScratch;

static void *lmbe_scratch_new(int nrows) {
    (void)nrows;                      /* K3's kernels are one row at a time */
    LmbeScratch *sc = (LmbeScratch *)calloc(1, sizeof *sc);
    sc->gate = falloc(lmbe_M.c.moe_inter);
    sc->up   = falloc(lmbe_M.c.moe_inter);
    sc->hz   = falloc(lmbe_M.c.latent);
    return sc;
}

static void lmbe_scratch_free(void *p) {
    LmbeScratch *sc = (LmbeScratch *)p;
    free(sc->gate); free(sc->up); free(sc->hz); free(sc);
}

/* The engine's OWN expert_apply, not a transcription of it.
 *
 * Its last line is `u[i] += wk*hz[i]`, so calling it with a zeroed output row
 * and wk = 1.0f leaves exactly hz[i] there: `0.0f + 1.0f*x` is x in IEEE 754,
 * for every x including denormals and signed zero. That gives the unweighted
 * expert output the chatter wants, and makes drift between local and remote
 * impossible by construction rather than by inspection. The router weight is
 * applied by the caller on the chatter, exactly as this engine applies it. */
static void lmbe_apply(const LmbeSlot *ec, int slot, const float *z, float *out,
                       int nrows, const float *w, void *p) {
    (void)w;                          /* this engine weights on the chatter */
    LmbeScratch *sc = (LmbeScratch *)p;
    Slot *s = (Slot *)ec;
    int LT = lmbe_M.c.latent;
    (void)slot;
    memset(out, 0, (size_t)nrows * LT * sizeof(float));
    for (int r = 0; r < nrows; r++)
        expert_apply(&lmbe_M, s, z + (int64_t)r * LT, 1.0f,
                     out + (int64_t)r * LT, sc->gate, sc->up, sc->hz);
}

#endif
