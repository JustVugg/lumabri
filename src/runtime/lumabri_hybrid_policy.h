/* Per-layer measured scheduling. All-local and split use the SAME expert
 * callback and router accumulation order. This is not a speed guarantee:
 * probes cost work, load varies, and the decision must remain reversible. */
#ifndef LUMABRI_HYBRID_POLICY_H
#define LUMABRI_HYBRID_POLICY_H
#include <stdint.h>
#include <math.h>
/* Household FP32 compatibility policy v1. This is an error envelope, NOT
 * bitwise identity or a guarantee of identical greedy tokens. The approved
 * OLMoE callback is evaluated against its local counterpart; cross-platform
 * libm/compiler rounding alone must not blacklist a donor. End-to-end oracle
 * tests remain a separate gate. No NaN/Inf, even matching ones, is accepted. */
#define LMB_HYBRID_FP32_ATOL 1e-6
#define LMB_HYBRID_FP32_RTOL 1e-5
enum { LMB_HYBRID_NUMERIC_REJECT = -1, LMB_HYBRID_NUMERIC_EXACT,
       LMB_HYBRID_NUMERIC_ROUNDING };
static inline int lmb_hybrid_numeric_check(const float *reference,
    const float *remote, int n, double *max_error) {
    int different = 0, compatible = 1;
    double largest = 0;
    if (max_error) *max_error = 0;
    if (!reference || !remote || n <= 0) return LMB_HYBRID_NUMERIC_REJECT;
    for (int i = 0; i < n; i++) {
        if (!isfinite(reference[i]) || !isfinite(remote[i])) {
            if (max_error) *max_error = INFINITY;
            return LMB_HYBRID_NUMERIC_REJECT;
        }
        double error = fabs((double)reference[i] - remote[i]);
        if (error > largest) largest = error;
        if (error > LMB_HYBRID_FP32_ATOL + LMB_HYBRID_FP32_RTOL * fabs((double)reference[i]))
            compatible = 0;
        if (error != 0) different = 1;
    }
    if (max_error) *max_error = largest;
    return !compatible ? LMB_HYBRID_NUMERIC_REJECT : different ?
        LMB_HYBRID_NUMERIC_ROUNDING : LMB_HYBRID_NUMERIC_EXACT;
}
enum { LMB_HYBRID_ADAPTIVE, LMB_HYBRID_FORCE_SPLIT, LMB_HYBRID_FORCE_LOCAL };
typedef struct {
    uint64_t rounds, samples[2]; /* 0 local, 1 split */
    double seconds[2], send_s, local_s, wait_s;
} LmbHybridTiming;
static inline int lmb_hybrid_choose(const LmbHybridTiming *t, int policy) {
    if (policy == LMB_HYBRID_FORCE_LOCAL) return 0;
    if (policy == LMB_HYBRID_FORCE_SPLIT) return 1;
    /* Interleave four samples of each to reduce warm-up/order bias. */
    if (t->samples[0] < 4 || t->samples[1] < 4) return !(t->rounds & 1);
    int split = t->seconds[1] < t->seconds[0] * 0.97;
    /* Periodically revisit the other path; never evict approved RAM. */
    return t->rounds % 64 == 0 ? !split : split;
}
static inline void lmb_hybrid_observe(LmbHybridTiming *t, int split,
    double total, double send, double local, double wait) {
    if (split < 0 || split > 1 || !isfinite(total) || total <= 0) return;
    if (!t->samples[split]) t->seconds[split] = total;
    else t->seconds[split] += 0.2 * (total - t->seconds[split]);
    t->samples[split]++; t->rounds++;
    t->send_s += send; t->local_s += local; t->wait_s += wait;
}
#endif
