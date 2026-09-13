/* Opt-in microbenchmark against an ALREADY APPROVED resident accelerator.
 * No allocations are requested from donors. Not a token/s benchmark.
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
            L.on = mode; L.hybrid_policy = LMB_HYBRID_FORCE_SPLIT;
            unsigned long long remote_before = L.calls;
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
                prompt, mode ? "split" : "native-local", counts[mode], prefill, decode_steps, decode);
            fflush(stdout);
            if (mode && (L.hybrid_numeric_failed ||
                L.calls - remote_before != (unsigned long long)covered * (np + decode_steps))) {
                fprintf(stderr, "GREEDY NOT VALIDATED: not every planned remote contribution was used\n");
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

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: bench MODEL BEGIN END THREADS\n"); return 2; }
    int begin = atoi(argv[2]), end = atoi(argv[3]), threads = atoi(argv[4]);
    if (begin < 0 || end <= begin || end > 512 || threads < 1 || threads > 256) return 2;
    if (!getenv("LUMABRI_HOME_HYBRID_ROUTES") || lmb_secure_init()) return 2;
    omp_set_num_threads(threads);
    Cfg config = {0}; load_cfg(&config, argv[1]);
    if (end > config.n_layers) return 2;
    int greedy = getenv("LMB_BENCH_GREEDY") != NULL;
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
    if (lumi_home_init(config.n_layers, config.n_experts, config.hidden, "olmoe/f32-int8/cpu-v1")) return 3;
    for (int layer = begin; layer < end; layer++) if (!L.layer_ok[layer]) return 3;
    if (greedy) return greedy_oracle(&m, argv[1]);
    float *x = falloc(config.hidden), *out = falloc(config.hidden), *oracle = falloc(config.hidden);
    const char *names[] = {"native-local", "callback-local", "split", "adaptive"};
    double elapsed[4] = {0}, max_abs = 0, max_rel = 0;
    unsigned samples[4] = {0}, mismatches = 0, rounding = 0;
    int inspect = getenv("LMB_BENCH_INSPECT_DIFF") != NULL;
    /* Inspection skips initial probes, but not periodic/non-finite checks.
     * Any rounding or fallback must exit nonzero in this strict diagnostic;
     * never mistake envelope compatibility for bit-identical execution. */
    if (inspect) memset(L.hybrid_probes, 4, sizeof L.hybrid_probes);
    /* Alternate paths and inputs, exclude first block from timings. Always
     * compare against the untouched native MoE result for that same input. */
    for (int repeat = 0; repeat < 17; repeat++) {
        for (int d = 0; d < config.hidden; d++) x[d] = (float)((d * 17 + repeat * 13) % 101 - 50) / 51.f;
        for (int layer = begin; layer < end; layer++) {
            L.on = 0;
            moe(&m, &m.L[layer], layer, x, 1, oracle);
            for (int j = 0; j < 4; j++) {
                int mode = (j + repeat) % 4;
                L.on = mode != 0;
                L.hybrid_policy = mode == 1 ? LMB_HYBRID_FORCE_LOCAL :
                    mode == 2 ? LMB_HYBRID_FORCE_SPLIT : LMB_HYBRID_ADAPTIVE;
                double start = lumi_now();
                moe(&m, &m.L[layer], layer, x, 1, out);
                double seconds = lumi_now() - start;
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
                if (repeat) { elapsed[mode] += seconds; samples[mode]++; }
            }
        }
    }
    for (int i = 0; i < 4; i++) printf("%s: %.3f ms/MoE-layer (%u samples)\n", names[i],
        1000 * elapsed[i] / samples[i], samples[i]);
    if (mismatches) printf("NUMERIC FAIL: %u differing results; max_abs=%g max_scaled=%g; diagnostic timings only\n",
        mismatches, max_abs, max_rel);
    else if (L.hybrid_numeric_failed)
        printf("NUMERIC FALLBACK PASS: native local results retained after incompatible remote probe; RPC calls=%llu\n", L.calls);
    else if (rounding) printf("NUMERIC ENVELOPE PASS: %u rounded results, max_abs=%g max_scaled=%g; "
        "remote calls=%llu; NOT bit-identical\n", rounding, max_abs, max_rel, L.calls);
    else printf("NUMERIC PASS: all modes bit-identical to native local; remote calls=%llu\n", L.calls);
    free(x); free(out); free(oracle);
    /* Process exit releases the diagnostic model; existing donors untouched. */
    return mismatches || (inspect && (rounding || L.hybrid_numeric_failed)) ? 4 : 0;
}
