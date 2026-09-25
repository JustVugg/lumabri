/* Opt-in service sweep or full-model oracle against an ALREADY APPROVED
 * resident accelerator. No allocations are requested from donors. Service
 * sweep results are per-layer timings, not a token/s benchmark.
 * Build with the prepared OLMoE include directory and the normal Segment
 * archive. No upstream files are edited. */
#define OLMOE_NO_MAIN
#include "olmoe.c"
#include <assert.h>

/* Full-model oracle, distinct from the synthetic per-layer timing loop.
 * Uses the actual tokenizer/template, fresh KV per run and greedy sampling.
 * It allocates a separate resident model; existing household state is not
 * changed. A passing finite corpus is NOT universal bitwise equivalence. */
static int greedy_oracle(Model *m, const char *directory) {
    int adaptive = getenv("LMB_BENCH_ADAPTIVE") != NULL;
    const char *prompts[] = {"Descrivi i cappelletti in due frasi.",
        "What is 17 plus 25? Explain briefly.",
        "Write a short Python function that returns the larger of two numbers."};
    Tok tokenizer; char path[4096];
    if (snprintf(path, sizeof path, "%s/tokenizer.json", directory) >= (int)sizeof path) return 2;
    tok_load(&tokenizer, path); g_temp = 0; g_pilot = 0;
    stops_arm_tok(&m->c, tok_id_of(&tokenizer, "|||IP_ADDRESS|||"), &tokenizer);
    m->max_t = 512;
    m->K = calloc(m->c.n_layers, sizeof *m->K);
    m->V = calloc(m->c.n_layers, sizeof *m->V);
    if (!m->K || !m->V) return 2;
    for (int i = 0; i < m->c.n_layers; i++) {
        m->K[i] = falloc((int64_t)m->c.n_heads * m->max_t * m->c.head_dim);
        m->V[i] = falloc((int64_t)m->c.n_heads * m->max_t * m->c.head_dim);
    }
    unsigned total = 0; uint64_t misses = m->miss;
    unsigned covered = 0;
    for (int layer = 0; layer < m->c.n_layers; layer++) if (L.layer_ok[layer]) covered++;
    for (unsigned prompt = 0; prompt < sizeof prompts / sizeof *prompts; prompt++) {
        char text[2048]; int ids[2048];
        int length = fmt_user_turn(text, sizeof text, prompts[prompt], 1);
        int np = length > 0 ? tok_encode(&tokenizer, text, length, ids, 2048) : -1;
        if (np < 1 || np + 32 > 512) return 2;
        int generated[2][32], counts[2] = {0};
        /* Reverse order on alternating prompts to expose state/order bias. */
        for (int j = 0; j < 2; j++) {
            int mode = (j + prompt) % 2;
            L.on = mode; L.hybrid_policy = adaptive ? LMB_HYBRID_ADAPTIVE : LMB_HYBRID_FORCE_SPLIT;
            unsigned long long remote_before = L.calls;
            unsigned long long rounds_before = L.hybrid_rounds, local_before = L.hybrid_local_calls;
            m->kv_len = 0;
            double start = lumi_now();
            float *logits = step(m, ids, np, 0);
            double prefill = lumi_now() - start, decode_start = lumi_now();
            unsigned decode_steps = 0;
            for (int k = 0; k < 32; k++) {
                int token = pick_tok(logits, m->c.vocab, -1);
                free(logits); logits = NULL;
                generated[mode][counts[mode]++] = token;
                if (is_stop(token) || k == 31) break;
                logits = step(m, &token, 1, np + k); decode_steps++;
            }
            double decode = lumi_now() - decode_start;
            printf("greedy prompt=%u mode=%s tokens=%d prefill=%.3fs decode_steps=%u decode=%.3fs\n",
                prompt, mode ? (adaptive ? "adaptive" : "split") : "native-local", counts[mode], prefill, decode_steps, decode);
            fflush(stdout);
            /* A failed split returns to the native caller without committing
             * a Hybrid round. Count complete rounds as well as contributions,
             * so an adaptive all-local decision is distinct from fallback. */
            unsigned long long expected = (unsigned long long)covered * (np + decode_steps);
            int forced = L.hybrid_remote_experts ? L.hybrid_remote_experts : 1;
            int limit = lmb_hybrid_remote_limit(m->c.topk);
            if (forced > limit) forced = limit;
            if (mode && (L.hybrid_numeric_failed ||
                L.hybrid_rounds - rounds_before != expected ||
                (!adaptive && L.calls - remote_before != expected * forced) ||
                L.calls - remote_before + L.hybrid_local_calls - local_before != expected * m->c.topk)) {
                fprintf(stderr, "GREEDY NOT VALIDATED: incomplete Hybrid rounds or local fallback\n");
                return 4;
            }
        }
        if (counts[0] != counts[1] || memcmp(generated[0], generated[1], counts[0] * sizeof(int))) {
            fprintf(stderr, "GREEDY FAIL: prompt=%u local_count=%d split_count=%d\n", prompt, counts[0], counts[1]);
            return 4;
        }
        total += counts[0];
    }
    tok_free(&tokenizer);
    if (m->miss != misses) { fprintf(stderr, "RESIDENCY FAIL: late expert loads\n"); return 4; }
    if (L.hybrid_numeric_failed || !L.calls) {
        fprintf(stderr, "GREEDY NOT VALIDATED: remote path absent or replaced by local fallback\n");
        return 4;
    }
    printf("GREEDY PASS: %u matching token IDs, 3 prompts; remote_calls=%llu rounding_probes=%llu; "
        "no late expert loads (not an OS no-swap assertion)\n", total, L.calls, L.hybrid_rounding_accepts);
    return 0;
}

#include "bench_block_verify.h"
#include "bench_causal_spec.h"

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: bench MODEL BEGIN END THREADS\n"); return 2; }
    int begin = atoi(argv[2]), end = atoi(argv[3]), threads = atoi(argv[4]);
    if (begin < 0 || end <= begin || end > 512 || threads < 1 || threads > 256) return 2;
    int causal = getenv("LMB_BENCH_CAUSAL") != NULL;
    int block = causal || getenv("LMB_BENCH_BLOCK") != NULL;
    if (!block && (!getenv("LUMABRI_HOME_HYBRID_ROUTES") || lmb_secure_init())) return 2;
    omp_set_num_threads(threads);
    Cfg config = {0}; load_cfg(&config, argv[1]);
    if (end > config.n_layers) return 2;
    int greedy = block || getenv("LMB_BENCH_GREEDY") != NULL;
    Model m;
    model_init_range(&m, argv[1], config.n_experts, 8,
        greedy ? 0 : begin, greedy ? config.n_layers : end, greedy, 0);
    int load_begin = greedy ? 0 : begin, load_end = greedy ? config.n_layers : end;
    /* Disjoint layer caches; expert_get protects its shared counters and
     * slot publication. Limit preparation parallelism independently of the
     * decode team. All reads finish before either measurement begins. */
    #pragma omp parallel for schedule(dynamic, 1) num_threads(threads < 4 ? threads : 4)
    for (int layer = load_begin; layer < load_end; layer++) {
        for (int e = 0; e < config.n_experts; e++) { Slot *slot; expert_get(&m, layer, e, &slot); }
        if (greedy) fprintf(stderr, "[oracle] layer %d resident: %d/%d experts\n",
            layer, config.n_experts, config.n_experts);
    }
    if (causal) return causal_oracle(&m, argv[1]);
    if (block) return block_oracle(&m, argv[1]);
    if (lumi_home_init(config.n_layers, config.n_experts, config.hidden, "olmoe/f32-int8/cpu-v1")) return 3;
    for (int layer = begin; layer < end; layer++) if (!L.layer_ok[layer]) return 3;
    if (greedy) return greedy_oracle(&m, argv[1]);
    float *x = falloc(config.hidden), *out = falloc(config.hidden), *oracle = falloc(config.hidden);
    int fanout = getenv("LMB_BENCH_FANOUT") != NULL;
    int limit = lmb_hybrid_remote_limit(config.topk);
    if (!limit) { fprintf(stderr, "Hybrid sweep requires top-k >= 2\n"); return 2; }
    char names[7][32] = {"native-local", "callback-local"};
    int counts[7] = {0}, modes = 2;
    const int candidates[] = {1, 2, 4, limit};
    for (unsigned i = 0; i < (fanout ? 4u : 1u); i++) {
        int n = fanout ? candidates[i] : (L.hybrid_remote_experts ? L.hybrid_remote_experts : 1);
        if (n > limit) n = limit;
        int duplicate = 0;
        for (int j = 2; j < modes; j++) if (counts[j] == n) duplicate = 1;
        if (duplicate) continue;
        counts[modes] = n;
        snprintf(names[modes++], sizeof names[0], "split-%d", n);
    }
    snprintf(names[modes++], sizeof names[0], "adaptive");
    double elapsed[7] = {0}, max_abs = 0, max_rel = 0;
    unsigned samples[7] = {0}, mismatches = 0, rounding = 0;
    /* Forced schedules must not train the adaptive contender. Give each
     * mode its own per-layer measurements, and time its warm-up separately. */
    LmbHybridTiming *timing = calloc((size_t)modes * config.n_layers, sizeof *timing);
    if (!timing) return 2;
    int warmup = fanout ? 4 * limit + 1 : 1;
    double warmup_start = lumi_now(), warmup_seconds = 0;
    int inspect = getenv("LMB_BENCH_INSPECT_DIFF") != NULL;
    /* Inspection skips initial probes, but not periodic/non-finite checks.
     * Any rounding or fallback must exit nonzero in this strict diagnostic;
     * never mistake envelope compatibility for bit-identical execution. */
    if (inspect) memset(L.hybrid_probes, 4, sizeof L.hybrid_probes);
    /* Alternate paths and inputs, report warm-up separately. Always
     * compare against the untouched native MoE result for that same input. */
    for (int repeat = 0; repeat < warmup + 16; repeat++) {
        if (repeat == warmup) warmup_seconds = lumi_now() - warmup_start;
        for (int d = 0; d < config.hidden; d++) x[d] = (float)((d * 17 + repeat * 13) % 101 - 50) / 51.f;
        for (int layer = begin; layer < end; layer++) {
            L.on = 0;
            moe(&m, &m.L[layer], layer, x, 1, oracle);
            for (int j = 0; j < modes; j++) {
                int mode = (j + repeat) % modes;
                L.on = mode != 0;
                L.hybrid_policy = mode == 1 ? LMB_HYBRID_FORCE_LOCAL :
                    mode == modes - 1 ? LMB_HYBRID_ADAPTIVE : LMB_HYBRID_FORCE_SPLIT;
                L.hybrid_remote_experts = counts[mode];
                L.hybrid_timing[layer] = timing[mode * config.n_layers + layer];
                unsigned long long before = L.hybrid_rounds;
                unsigned long long calls_before = L.calls;
                double start = lumi_now();
                moe(&m, &m.L[layer], layer, x, 1, out);
                double seconds = lumi_now() - start;
                timing[mode * config.n_layers + layer] = L.hybrid_timing[layer];
                if (mode && (L.hybrid_rounds != before + 1 || L.hybrid_numeric_failed ||
                    (mode != modes - 1 && L.calls - calls_before != (unsigned)counts[mode]))) {
                    fprintf(stderr, "SWEEP NOT VALIDATED: failed Hybrid round or numeric fallback\n");
                    return 4;
                }
                if (memcmp(out, oracle, config.hidden * sizeof(float))) {
                    int compatible = lmb_hybrid_numeric_check(oracle, out, config.hidden, NULL) >= 0;
                    if (compatible) rounding++; else mismatches++;
                    for (int d = 0; d < config.hidden; d++) {
                        double diff = fabs((double)out[d] - oracle[d]);
                        double rel = diff / fmax(1.0, fabs(oracle[d]));
                        if (diff > max_abs) max_abs = diff;
                        if (rel > max_rel) max_rel = rel;
                    }
                    if (!inspect && !compatible) {
                        fprintf(stderr, "NUMERIC MISMATCH: mode=%s layer=%d input=%d max_abs=%g max_scaled=%g\n",
                            names[mode], layer, repeat, max_abs, max_rel);
                        return 4;
                    }
                }
                if (repeat >= warmup) { elapsed[mode] += seconds; samples[mode]++; }
            }
        }
    }
    printf("sweep warmup: %.3f s (%d rounds per mode/layer; excluded from service means, not free)\n",
        warmup_seconds, warmup);
    for (int i = 0; i < modes; i++) printf("%s: %.3f ms/MoE-layer (%u samples)\n", names[i],
        1000 * elapsed[i] / samples[i], samples[i]);
    for (int layer = begin; layer < end; layer++)
        printf("adaptive layer=%d best_remote=%d\n", layer,
            lmb_hybrid_best(&timing[(modes - 1) * config.n_layers + layer]));
    if (mismatches) printf("NUMERIC FAIL: %u differing results; max_abs=%g max_scaled=%g; diagnostic timings only\n",
        mismatches, max_abs, max_rel);
    else if (rounding) printf("NUMERIC ENVELOPE PASS: %u rounded results, max_abs=%g max_scaled=%g; "
        "remote calls=%llu; NOT bit-identical\n", rounding, max_abs, max_rel, L.calls);
    else printf("NUMERIC PASS: all modes bit-identical to native local; remote calls=%llu\n", L.calls);
    free(x); free(out); free(oracle); free(timing);
    /* Process exit releases the diagnostic model; existing donors untouched. */
    return mismatches || (inspect && rounding) ? 4 : 0;
}
