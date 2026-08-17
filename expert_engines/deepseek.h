/* expert_engines/deepseek.h — the DeepSeek V4 glue.
 *
 * The odd one out in three ways, all of which reach the wire:
 *
 *   the weight is not a scale   coli_v4_expert_forward_ref folds the router
 *                               weight in BEFORE the down projection and
 *                               rounds the product to bf16. So `w · e(x)` is
 *                               not what the engine computes, the weight has
 *                               to arrive with the activation, and what goes
 *                               back is already weighted.
 *   no dense weights here       V4 has a standalone expert store that opens
 *                               straight off the model directory, so a peer
 *                               holds experts and nothing else — which is
 *                               what a peer should hold anyway. The other
 *                               four glues pay for a full model_init.
 *   the store owns residency    lookup/release is already an LRU with pins,
 *                               so an LmbeSlot here is just the key and
 *                               `--cache N` becomes the store's byte budget.
 *
 * The expert math is the engine's own function, called with the engine's own
 * view: nothing is transcribed, so nothing can drift.
 */
#ifndef LMBE_DEEPSEEK_H
#define LMBE_DEEPSEEK_H

/* Two colibri layouts, one glue:
 *
 *  LMBE_DS_MULTIFILE — current colibri ships DeepSeek V4 as deepseek_v4.c
 *    compiled once per COLI_V4_UNIT_* into separate objects (see
 *    Makefile.deepseek-v4.units). The Makefile builds those units and links
 *    them, so here we only need the declarations from the engine's internal
 *    header; the definitions arrive at link time.
 *
 *  otherwise — the older single amalgamated deepseek.c, included directly
 *    (main renamed textually into build/deepseek_noentry.c because that file
 *    undefines `main` halfway through). colibri itself is untouched either way.
 */
#ifdef LMBE_DS_MULTIFILE
#include "deepseek_v4_internal.h"
#else
#include "deepseek_noentry.c"
#endif

typedef struct { int layer, eid; } LmbeSlot;

static ColiDeepSeekV4Config lmbe_cfg;
static ColiExpertStore *lmbe_store;

static const char *lmbe_engine_name(void) { return "deepseek_v4"; }
static int lmbe_effective_bits(int bits) { (void)bits; return 0; }

/* `cap` is expert slots; the store wants bytes, so it is scaled by the
 * record size the store itself reports back through its stats. Before that
 * is known, ask for cap × a generous per-expert estimate and let the store
 * clamp: it refuses anything under its own minimum and says so. */
static void lmbe_open(const char *dir, int cap, int bits) {
    (void)bits;                       /* V4 experts are fp4 on disk, as shipped */
    char err[512] = "";
    if (coli_v4_config_load(&lmbe_cfg, dir, err, sizeof err)) {
        fprintf(stderr, "[lumabri] %s\n", err[0] ? err : "cannot read the V4 config");
        exit(1);
    }
    /* One fp4 expert is three matrices of moe_intermediate × hidden at half a
     * byte, plus the block scales — a fifth over is enough headroom, and the
     * store refuses anything under six live experts per layer (top-k), so
     * that is the floor `--cache` cannot go below. */
    uint64_t per_expert = (uint64_t)lmbe_cfg.moe_intermediate_size *
                          (uint64_t)lmbe_cfg.hidden_size * 3 / 2;
    per_expert += per_expert / 5;
    int slots = cap > 0 ? cap : 8;
    if (slots < lmbe_cfg.num_experts_per_tok) slots = lmbe_cfg.num_experts_per_tok;
    ColiDeepSeekV4ExpertStoreOptions o = {
        .model_dir = dir,
        .layers = lmbe_cfg.num_hidden_layers,
        .experts_per_layer = lmbe_cfg.n_routed_experts,
        .cache_bytes = per_expert * (uint64_t)slots *
                       (uint64_t)lmbe_cfg.num_hidden_layers,
        .pin_slots_per_layer = -1,
        .repin_interval = 0,
    };
    if (coli_deepseek_v4_expert_store_open(&o, &lmbe_store, err, sizeof err)) {
        fprintf(stderr, "[lumabri] expert store: %s\n", err);
        exit(1);
    }
}

static int lmbe_n_slots(void)   { return lmbe_cfg.num_hidden_layers; }
static int lmbe_n_experts(void) { return lmbe_cfg.n_routed_experts; }
static int lmbe_hidden(void)    { return lmbe_cfg.hidden_size; }
static int lmbe_inter(void)     { return lmbe_cfg.moe_intermediate_size; }
static int lmbe_routed(int slot) { (void)slot; return 1; }   /* every V4 layer routes */

static void lmbe_slot_init(LmbeSlot *s) { s->layer = -1; s->eid = -1; }

/* Nothing to load: the store is the cache, with its own LRU and pins. The
 * node's own residency modes would only duplicate it, so a slot here is the
 * key and the lookup happens at apply time. */
static void lmbe_slot_load(int slot, int eid, LmbeSlot *s) {
    s->layer = slot; s->eid = eid;
}

static void *lmbe_scratch_new(int nrows) { (void)nrows; return NULL; }
static void lmbe_scratch_free(void *p)   { (void)p; }

/* The engine's own expert forward, with the engine's own view, and with the
 * router weight it needs — which is why the wire carries it. */
static void lmbe_apply(const LmbeSlot *s, int slot, const float *x, float *out,
                       int nrows, const float *w, void *p) {
    (void)p;
    if (!w) {
        fprintf(stderr, "[lumabri] V4 expert called without router weights: the "
                        "chatter is speaking an older dialect\n");
        exit(1);
    }
    int d = lmbe_cfg.hidden_size;
    ColiExpertView view;
    if (coli_expert_lookup(lmbe_store,
                           (ColiExpertKey){slot, s->eid}, &view)) {
        fprintf(stderr, "[lumabri] cannot lease layer %d expert %d\n", slot, s->eid);
        exit(1);
    }
    for (int r = 0; r < nrows; r++)
        if (coli_v4_expert_forward_ref(out + (size_t)r * d, &view,
                                       x + (size_t)r * d, w[r],
                                       lmbe_cfg.swiglu_limit)) {
            coli_expert_release(lmbe_store, &view);
            fprintf(stderr, "[lumabri] expert forward failed at layer %d expert %d\n",
                    slot, s->eid);
            exit(1);
        }
    coli_expert_release(lmbe_store, &view);
}

#endif
