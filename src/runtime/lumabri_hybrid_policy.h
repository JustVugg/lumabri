/* Per-layer measured scheduling. All-local and split use the SAME expert
 * callback and router accumulation order. This is not a speed guarantee:
 * probes cost work, load varies, and the decision must remain reversible. */
#ifndef LUMABRI_HYBRID_POLICY_H
#define LUMABRI_HYBRID_POLICY_H
#include <stdint.h>
#include <math.h>
#include <string.h>
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
/* Keep at least one local expert and bound concurrent sockets/queued work.
 * These are alternative schedules within an already approved layer, never
 * permission to load additional weights or contact another donor. */
#define LMB_HYBRID_MAX_REMOTE 8
typedef struct {
    uint64_t rounds, samples[LMB_HYBRID_MAX_REMOTE + 1];
    uint64_t retry_after[LMB_HYBRID_MAX_REMOTE + 1];
    double seconds[LMB_HYBRID_MAX_REMOTE + 1], send_s, local_s, wait_s;
    int top_k, last_remote;
} LmbHybridTiming;
static inline int lmb_hybrid_remote_limit(int top_k) {
    return top_k <= 1 ? 0 : top_k > LMB_HYBRID_MAX_REMOTE ?
        LMB_HYBRID_MAX_REMOTE : top_k - 1;
}
static inline int lmb_hybrid_best(const LmbHybridTiming *t) {
    int best = 0;
    if (!t->samples[0]) return 0;
    for (int r = 1; r <= lmb_hybrid_remote_limit(t->top_k); r++) {
        if (t->samples[r] < 2 || t->rounds < t->retry_after[r]) continue;
        if (t->seconds[r] < t->seconds[0] * 0.97 &&
            (!best || t->seconds[r] < t->seconds[best])) best = r;
    }
    return best;
}
/* Returns a COUNT, not a split/all-local boolean. Observe actual completion
 * (including connection and queue costs); counters belong to that count. */
static inline int lmb_hybrid_choose(LmbHybridTiming *t, int policy,
                                    int top_k, int forced_remote) {
    if (t->top_k != top_k) { memset(t, 0, sizeof *t); t->top_k = top_k; }
    int limit = lmb_hybrid_remote_limit(top_k);
    if (!limit || policy == LMB_HYBRID_FORCE_LOCAL) return 0;
    if (policy == LMB_HYBRID_FORCE_SPLIT)
        return forced_remote < 1 ? 1 : forced_remote > limit ? limit : forced_remote;
    /* Two samples per alternative, each preceded by a fresh local baseline.
     * Bounded to at most 4*limit+1 rounds; no unbounded online grid search. */
    if (!t->samples[0]) return 0;
    for (int r = 1; r <= limit; r++)
        if (t->samples[r] < 2 && t->rounds >= t->retry_after[r])
            return t->last_remote ? 0 : r;
    int best = lmb_hybrid_best(t);
    /* Revisit a different allocation periodically, including all-local when
     * a remote count currently wins. Never evict approved resident weights. */
    if (t->rounds && t->rounds % 64 == 0) {
        int probe = (int)((t->rounds / 64 - 1) % (uint64_t)(limit + 1));
        for (int i = 0; i <= limit; i++, probe = (probe + 1) % (limit + 1))
            if (probe != best && t->rounds >= t->retry_after[probe]) return probe;
    }
    return best;
}
static inline void lmb_hybrid_observe(LmbHybridTiming *t, int remote,
    double total, double send, double local, double wait) {
    if (remote < 0 || remote > lmb_hybrid_remote_limit(t->top_k) ||
        !isfinite(total) || total <= 0 || !isfinite(send) || send < 0 ||
        !isfinite(local) || local < 0 || !isfinite(wait) || wait < 0) return;
    if (!t->samples[remote]) t->seconds[remote] = total;
    else t->seconds[remote] += 0.2 * (total - t->seconds[remote]);
    t->samples[remote]++; t->rounds++; t->last_remote = remote;
    t->send_s += send; t->local_s += local; t->wait_s += wait;
}
static inline void lmb_hybrid_failed(LmbHybridTiming *t, int remote) {
    if (remote < 1 || remote > lmb_hybrid_remote_limit(t->top_k)) return;
    /* Do not record failure as a fast sample or retry an unsampled count on
     * every token. Existing transport/identity failure rules still apply. */
    t->rounds++; t->last_remote = remote;
    t->retry_after[remote] = t->rounds + 128;
    t->samples[remote] = 0; t->seconds[remote] = 0;
}
#endif
