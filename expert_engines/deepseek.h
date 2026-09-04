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
#define LMBE_STORE_OWNS_RESIDENCY 1

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
static char lmbe_model_dir[4096];
static uint64_t lmbe_resident_nbytes;

static const char *lmbe_engine_name(void) { return "deepseek_v4"; }
static int lmbe_effective_bits(int bits) { (void)bits; return 0; }

/* The store wants bytes and thinks per layer: colibri turns `cache_bytes`
 * into `cache_bytes / (layers * record)` slots for EVERY layer. Lumabri's
 * `--cache N` (and `serve --exec-cache N`) means N experts for the whole
 * node, like every other glue, so N is spread over the layers here. The
 * old code multiplied by the layer count instead: `--exec-cache 128` on the
 * 43-layer V4-Flash was 5504 experts (~69 GB) and 1800 was the entire
 * expert set, which is how a 64 GB server filled its RAM and died. */
static void lmbe_open_per_layer(const char *dir, int slots_per_layer) {
    char err[512] = "";
    if (coli_v4_config_load(&lmbe_cfg, dir, err, sizeof err)) {
        fprintf(stderr, "[lumabri] %s\n", err[0] ? err : "cannot read the V4 config");
        exit(1);
    }
    if (dir != lmbe_model_dir)
        snprintf(lmbe_model_dir, sizeof lmbe_model_dir, "%s", dir);
    /* One fp4 expert is three matrices of moe_intermediate × hidden at half a
     * byte, plus the block scales — a fifth over is enough headroom, and the
     * store refuses anything under six live experts per layer (top-k), so
     * that is the floor the budget cannot go below. */
    uint64_t per_expert = (uint64_t)lmbe_cfg.moe_intermediate_size *
                          (uint64_t)lmbe_cfg.hidden_size * 3 / 2;
    per_expert += per_expert / 5;
    int slots = slots_per_layer > 0 ? slots_per_layer : 8;
    if (slots < lmbe_cfg.num_experts_per_tok) slots = lmbe_cfg.num_experts_per_tok;
    if (slots > lmbe_cfg.n_routed_experts) slots = lmbe_cfg.n_routed_experts;
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
    fprintf(stderr, "[lumabri] expert store: %d slot%s per layer × %d layers "
                    "= %d experts, up to %.1f GB of RAM\n",
            slots, slots == 1 ? "" : "s", lmbe_cfg.num_hidden_layers,
            slots * lmbe_cfg.num_hidden_layers,
            (double)o.cache_bytes / 1e9);
}

/* `cap` is the node's total expert slots (`--cache N`); 0 means a small
 * default. It is divided over the layers, rounding up, and the top-k floor
 * per layer means a V4-Flash node holds at least 6 × 43 = 258 experts. */
static void lmbe_open(const char *dir, int cap, int bits) {
    (void)bits;                       /* V4 experts are fp4 on disk, as shipped */
    char err[512] = "";
    if (coli_v4_config_load(&lmbe_cfg, dir, err, sizeof err)) {
        fprintf(stderr, "[lumabri] %s\n", err[0] ? err : "cannot read the V4 config");
        exit(1);
    }
    int layers = lmbe_cfg.num_hidden_layers > 0 ? lmbe_cfg.num_hidden_layers : 1;
    int per_layer = cap > 0 ? (cap + layers - 1) / layers : 0;
    lmbe_open_per_layer(dir, per_layer);
}

/* READY means every assigned key already has a RAM slot. Re-open Colibri's
 * native store with enough slots for the busiest assigned layer, then warm
 * every key through its public lease contract before EREG can be sent. */
static int lmbe_resident_prepare(const uint8_t *holds, int layers,
                                 int experts, int nholds) {
    int max_per_layer = 0;
    for (int l = 0; l < layers; l++) {
        int n = 0;
        for (int e = 0; e < experts; e++) n += holds[l * experts + e] != 0;
        if (n > max_per_layer) max_per_layer = n;
    }
    if (max_per_layer < lmbe_cfg.num_experts_per_tok)
        max_per_layer = lmbe_cfg.num_experts_per_tok;
    if (lmbe_store && lmbe_store->ops && lmbe_store->ops->destroy)
        lmbe_store->ops->destroy(lmbe_store);
    lmbe_store = NULL;
    lmbe_open_per_layer(lmbe_model_dir, max_per_layer);
    for (int l = 0; l < layers; l++)
        for (int e = 0; e < experts; e++) {
            if (!holds[l * experts + e]) continue;
            ColiExpertView view = {0};
            if (coli_expert_lookup(lmbe_store, (ColiExpertKey){l, e}, &view))
                return -1;
            coli_expert_release(lmbe_store, &view);
        }
    ColiExpertStoreStats stats = {0};
    if (lmbe_store->ops && lmbe_store->ops->stats)
        lmbe_store->ops->stats(lmbe_store, &stats);
    lmbe_resident_nbytes = stats.resident_bytes;
#ifdef COLI_V4_GPU_TIER
    /* Ask the tier which of the assigned experts already carry GPU mirrors.
     * peek never uploads, so this measures rather than provokes residency. */
    lmbe_vram_nbytes = 0;
    if (lmbe_store->gpu)
        for (int l = 0; l < layers; l++)
            for (int e = 0; e < experts; e++) {
                ColiExpertView view = {0};
                if (!holds[l * experts + e]) continue;
                if (coli_expert_lookup(lmbe_store, (ColiExpertKey){l, e}, &view))
                    continue;
                if (!coli_v4_gpu_expert_peek(lmbe_store, &view) &&
                    view.gate.gpu && view.up.gpu && view.down.gpu)
                    lmbe_vram_nbytes += lmbe_expert_bytes();
                coli_expert_release(lmbe_store, &view);
            }
#endif
    return nholds > 0 && stats.resident_bytes > 0 ? 0 : -1;
}

static uint64_t lmbe_resident_bytes(void) { return lmbe_resident_nbytes; }

/* Experts this node keeps mirrored in VRAM. Colibri's V4 GPU tier attaches
 * fp4 mirrors to a view on lookup and reports residency through
 * coli_v4_gpu_expert_peek, so the count is taken once per resident-prepare
 * over the assigned keys: a peek never uploads, it only answers. Without
 * the GPU tier compiled in, or without a card, this is zero and the node
 * advertises RAM residency exactly as before. */
static uint64_t lmbe_vram_nbytes;
static uint64_t lmbe_vram_bytes(void) { return lmbe_vram_nbytes; }
#define LMBE_VRAM_BYTES lmbe_vram_bytes

/* set by lmbe_apply when it could not compute; the node turns it into a
 * refused call instead of a dead process */
static __thread int lmbe_apply_failed;
#define LMBE_APPLY_MAY_FAIL 1

/* Warm one expert into the store's RAM cache (its own LRU, pins included)
 * so the compute gate is never held across a disk read. A miss here is
 * not fatal: apply will look it up again and report the real error. */
static void lmbe_touch(int layer, int eid) {
    ColiExpertView view = {0};
    if (!lmbe_store) return;
    if (coli_expert_lookup(lmbe_store, (ColiExpertKey){layer, eid}, &view)) return;
    coli_expert_release(lmbe_store, &view);
}

/* fp4 expert: three moe_intermediate × hidden matrices at half a byte plus
 * block scales; the same figure lmbe_open budgets with. */
static uint64_t lmbe_expert_bytes(void) {
    uint64_t per_expert = (uint64_t)lmbe_cfg.moe_intermediate_size *
                          (uint64_t)lmbe_cfg.hidden_size * 3 / 2;
    return per_expert + per_expert / 5;
}
#define LMBE_EXPERT_BYTES lmbe_expert_bytes

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
    /* A lease can fail transiently: a prefill burst pins every slot of one
     * layer for a moment. That used to exit(1) the whole executor — the
     * origin died mid-reply and every chatter waited for the restart. Wait
     * a little for a slot, then fail THIS call (the chatter retries on the
     * next replica or later), never the process. */
    int leased = 0;
    for (int attempt = 0; attempt < 40 && !leased; attempt++) {
        if (!coli_expert_lookup(lmbe_store, (ColiExpertKey){slot, s->eid}, &view)) leased = 1;
        else usleep(5000);
    }
    if (!leased) {
        fprintf(stderr, "[lumabri] cannot lease layer %d expert %d after 200 ms: "
                        "refusing this call\n", slot, s->eid);
        lmbe_apply_failed = 1;
        return;
    }
    /* All the rows of one call in one pass: the batch kernel streams each
     * expert matrix once for every row, where the per-row loop re-read the
     * 12.6 MB expert for each of them — a layer round with 8 rows cost 7.6×
     * one with 1 (measured with swarm_rows_bench), so a 64-row prefill call
     * cost 64 expert reads. It is the kernel the engine itself uses for its
     * batched prefill (count > 1 → batch_ref), with the same numerical
     * contract; hot rows16 experts fall back to the scalar path inside it. */
    /* Measured on the origin (swarm_rows_bench, ms per layer round): the
     * scalar kernel is ~15 ms per row and parallel inside; the batch kernel
     * is a reference implementation with ~50 ms of fixed cost that pays off
     * between 4 and 8 rows (8 rows: 113 → 65 ms) and degrades past 8
     * (16 rows: 300 ms). So: rows one by one below four, the batch kernel in
     * blocks of eight from four on. */
    int failed = 0;
    if (nrows < 4) {
        for (int r = 0; r < nrows && !failed; r++)
            failed = coli_v4_expert_forward_ref(out + (size_t)r * d, &view,
                                                x + (size_t)r * d, w[r],
                                                lmbe_cfg.swiglu_limit);
    } else {
        for (int at = 0; at < nrows && !failed; at += 8) {
            int n = nrows - at < 8 ? nrows - at : 8;
            if (n == 1)
                failed = coli_v4_expert_forward_ref(out + (size_t)at * d, &view,
                                                    x + (size_t)at * d, w[at],
                                                    lmbe_cfg.swiglu_limit);
            else
                failed = coli_v4_expert_forward_batch_ref(out + (size_t)at * d, &view,
                                                          x + (size_t)at * d, w + at,
                                                          n, lmbe_cfg.swiglu_limit);
        }
    }
    if (failed) {
        coli_expert_release(lmbe_store, &view);
        fprintf(stderr, "[lumabri] expert forward failed at layer %d expert %d: "
                        "refusing this call\n", slot, s->eid);
        lmbe_apply_failed = 1;              /* ERR to the chatter, not exit */
        return;
    }
    coli_expert_release(lmbe_store, &view);
}

#endif
