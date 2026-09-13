/* Opt-in microbenchmark against an ALREADY APPROVED resident accelerator.
 * No allocations are requested from donors. Not a token/s benchmark.
 * Build with the prepared OLMoE include directory and the normal Segment
 * archive. No upstream files are edited. */
#define OLMOE_NO_MAIN
#include "olmoe.c"
#include <assert.h>

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: bench MODEL BEGIN END THREADS\n"); return 2; }
    int begin = atoi(argv[2]), end = atoi(argv[3]), threads = atoi(argv[4]);
    if (begin < 0 || end <= begin || end > 512 || threads < 1 || threads > 256) return 2;
    if (!getenv("LUMABRI_HOME_HYBRID_ROUTES") || lmb_secure_init()) return 2;
    omp_set_num_threads(threads);
    Cfg config = {0}; load_cfg(&config, argv[1]);
    if (end > config.n_layers) return 2;
    Model m;
    model_init_range(&m, argv[1], config.n_experts, 8, begin, end, 0, 0);
    for (int layer = begin; layer < end; layer++)
        for (int e = 0; e < config.n_experts; e++) { Slot *slot; expert_get(&m, layer, e, &slot); }
    if (lumi_home_init(config.n_layers, config.n_experts, config.hidden, "olmoe/f32-int8/cpu-v1")) return 3;
    for (int layer = begin; layer < end; layer++) if (!L.layer_ok[layer]) return 3;
    float *x = falloc(config.hidden), *out = falloc(config.hidden), *oracle = falloc(config.hidden);
    const char *names[] = {"native-local", "callback-local", "split", "adaptive"};
    double elapsed[4] = {0}, max_abs = 0, max_rel = 0; unsigned samples[4] = {0}, mismatches = 0;
    int inspect = getenv("LMB_BENCH_INSPECT_DIFF") != NULL;
    /* Inspection deliberately bypasses the runtime smoke-test fallback to
     * quantify raw cross-machine differences. It must exit nonzero for any
     * mismatch; never use this option to certify production execution. */
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
                    mismatches++;
                    for (int d = 0; d < config.hidden; d++) {
                        double diff = fabs((double)out[d] - oracle[d]);
                        double rel = diff / fmax(1.0, fabs(oracle[d]));
                        if (diff > max_abs) max_abs = diff;
                        if (rel > max_rel) max_rel = rel;
                    }
                    if (!inspect) {
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
    else printf("NUMERIC PASS: all modes bit-identical to native local; remote calls=%llu\n", L.calls);
    free(x); free(out); free(oracle);
    /* Process exit releases the diagnostic model; existing donors untouched. */
    return mismatches ? 4 : 0;
}
