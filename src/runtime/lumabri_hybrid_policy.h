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
/* This wider bound is only a WORK gate, never an acceptance threshold.
 * A borderline pair can be checked against an adapter's independent, more
 * precise reference. Gross errors and nonfinite values fail without work. */
static inline int lmb_hybrid_reference_candidate(const float *local,
    const float *remote, int n) {
    if (!local || !remote || n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        if (!isfinite(local[i]) || !isfinite(remote[i])) return 0;
        double limit = LMB_HYBRID_FP32_ATOL + LMB_HYBRID_FP32_RTOL *
            fmax(fabs((double)local[i]), fabs((double)remote[i]));
        if (fabs((double)local[i] - remote[i]) > 2 * limit) return 0;
    }
    return 1;
}
/* Each result must independently meet the ORIGINAL v1 envelope. This is a
 * numerical smoke test, not a proof of bitwise or whole-model equivalence. */
static inline int lmb_hybrid_reference_check(const float *reference,
    const float *local, const float *remote, int n) {
    return lmb_hybrid_numeric_check(reference, local, n, NULL) >= 0 &&
           lmb_hybrid_numeric_check(reference, remote, n, NULL) >= 0;
}
enum { LMB_HYBRID_ADAPTIVE, LMB_HYBRID_FORCE_SPLIT, LMB_HYBRID_FORCE_LOCAL };
/* Keep at least one local expert and bound concurrent sockets/queued work.
 * These are alternative schedules within an already approved layer, never
 * permission to load additional weights or contact another donor. */
#define LMB_HYBRID_MAX_REMOTE 8
#define LMB_HYBRID_PROBE_INTERVAL 16
#define LMB_HYBRID_PROBE_MAX_INTERVAL 512
#define LMB_HYBRID_PROBE_COST_FRACTION 0.03
#define LMB_HYBRID_LOCAL_REFRESH 8
typedef struct {
    uint64_t rounds, samples[LMB_HYBRID_MAX_REMOTE + 1];
    uint64_t retry_after[LMB_HYBRID_MAX_REMOTE + 1];
    uint64_t local_at, next_probe, validated_at, completed, remote_rounds, failures;
    double seconds[LMB_HYBRID_MAX_REMOTE + 1], send_s, local_s, wait_s;
    double last_local_s, wall_s, validation_s, merge_s, failed_s;
    int top_k, last_remote, probe_cursor;
} LmbHybridTiming;
static inline int lmb_hybrid_remote_limit(int top_k) {
    return top_k <= 1 ? 0 : top_k > LMB_HYBRID_MAX_REMOTE ?
        LMB_HYBRID_MAX_REMOTE : top_k - 1;
}
/* Pacing changes which rounds use peers. An exact rounds%64 check could then
 * miss every remote round. Validate the next remote result once due instead. */
static inline int lmb_hybrid_validation_due(const LmbHybridTiming *t, unsigned probes) {
    return probes < 4 || t->rounds - t->validated_at >= 64;
}
static inline int lmb_hybrid_best(const LmbHybridTiming *t) {
    int best = 0;
    if (!t->samples[0]) return 0;
    /* A slow startup sample must not keep a donor on the critical path once
     * the local machine is fast again. Keep the EWMA, but also require a win
     * against the most recent local observation. This is not a latency SLA. */
    double baseline = t->seconds[0];
    if (t->last_local_s > 0 && t->last_local_s < baseline) baseline = t->last_local_s;
    for (int r = 1; r <= lmb_hybrid_remote_limit(t->top_k); r++) {
        if (t->samples[r] < 2 || t->rounds < t->retry_after[r]) continue;
        if (t->seconds[r] < baseline * 0.97 &&
            (!best || t->seconds[r] < t->seconds[best])) best = r;
    }
    return best;
}
/* Amortize an unsuccessful probe against measured local service, rather
 * than charging every sixteenth token for an expensive new connection.
 * This is a bounded cooldown, NOT a 3% overhead guarantee: startup, jitter
 * and the finite rediscovery cap still cost real work. */
static inline uint64_t lmb_hybrid_probe_gap(const LmbHybridTiming *t, double wall) {
    if (!t->samples[0]) return LMB_HYBRID_PROBE_INTERVAL;
    double baseline = t->seconds[0];
    if (t->last_local_s > 0 && t->last_local_s < baseline) baseline = t->last_local_s;
    if (!isfinite(wall) || !isfinite(baseline) || baseline <= 0)
        return LMB_HYBRID_PROBE_MAX_INTERVAL;
    double excess = wall > baseline ? wall - baseline : 0;
    double gap = excess / (baseline * LMB_HYBRID_PROBE_COST_FRACTION);
    if (!isfinite(gap) || gap >= LMB_HYBRID_PROBE_MAX_INTERVAL)
        return LMB_HYBRID_PROBE_MAX_INTERVAL;
    if (gap <= LMB_HYBRID_PROBE_INTERVAL) return LMB_HYBRID_PROBE_INTERVAL;
    uint64_t whole = (uint64_t)gap; /* finite, positive and bounded by 512 */
    return whole + (gap > (double)whole);
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
    /* Bootstrap only the smallest split, with two fresh local references.
     * Do not charge the first response for an entire fan-out sweep. Other
     * counts are still explored (twice each), at a bounded cadence below. */
    if (!t->samples[0]) return 0;
    if (t->samples[1] < 2 && !t->retry_after[1])
        return t->last_remote ? 0 : 1;
    int best = lmb_hybrid_best(t);
    if (best && t->rounds - t->local_at >= LMB_HYBRID_LOCAL_REFRESH) return 0;
    uint64_t due = t->next_probe ? t->next_probe : LMB_HYBRID_PROBE_INTERVAL;
    if (t->rounds >= due) {
        int probe = t->probe_cursor ? t->probe_cursor : (limit > 1 ? 2 : 1);
        for (int i = 0; i < limit; i++, probe = probe % limit + 1) {
            if (probe == best || t->rounds < t->retry_after[probe]) continue;
            /* Pair a probe with a local observation, even when a different
             * remote schedule currently wins. Approval/residency unchanged. */
            return t->last_remote ? 0 : probe;
        }
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
    uint64_t due = t->next_probe ? t->next_probe : LMB_HYBRID_PROBE_INTERVAL;
    t->samples[remote]++; t->rounds++; t->last_remote = remote;
    if (!remote) { t->local_at = t->rounds; t->last_local_s = total; }
    else if ((remote == 1 && t->samples[1] == 2) || t->rounds > due) {
        t->next_probe = t->rounds + lmb_hybrid_probe_gap(t, total);
        t->probe_cursor = t->samples[remote] < 2 ? remote :
            remote % lmb_hybrid_remote_limit(t->top_k) + 1;
    }
    t->send_s += send; t->local_s += local; t->wait_s += wait;
}
/* Non-overlapping coordinator phases. wait is only the collection phase,
 * NOT network RTT or donor compute. Validation is included in wall time but
 * excluded from the service EWMA: the initial four probes are one-off work.
 * The end-to-end benchmark must include those probes, never subtract them. */
static inline void lmb_hybrid_record(LmbHybridTiming *t, int remote,
    double send, double local, double wait, double validation, double merge) {
    double phase[] = {send, local, wait, validation, merge};
    double wall = 0;
    for (unsigned i = 0; i < sizeof phase / sizeof *phase; i++) {
        if (!isfinite(phase[i]) || phase[i] < 0) return;
        wall += phase[i];
    }
    if (!isfinite(wall)) return;
    uint64_t before = t->rounds, probe_before = t->next_probe;
    lmb_hybrid_observe(t, remote, send + local + wait + merge, send, local, wait);
    if (t->rounds == before) return;
    /* Include exceptional reference work in the cooldown too. No latency is
     * subtracted from the end-to-end benchmark or the wall-time counters. */
    if (remote && t->next_probe != probe_before)
        t->next_probe = t->rounds + lmb_hybrid_probe_gap(t, wall);
    t->completed++; t->remote_rounds += remote > 0;
    t->wall_s += wall; t->validation_s += validation; t->merge_s += merge;
}
static inline void lmb_hybrid_failed(LmbHybridTiming *t, int remote, double elapsed) {
    if (remote < 0 || remote > lmb_hybrid_remote_limit(t->top_k) ||
        !isfinite(elapsed) || elapsed < 0) return;
    t->failures++; t->failed_s += elapsed;
    if (!remote) return;
    /* Do not record failure as a fast sample or retry an unsampled count on
     * every token. Existing transport/identity failure rules still apply. */
    t->rounds++; t->last_remote = remote;
    t->retry_after[remote] = t->rounds + 128;
    t->samples[remote] = 0; t->seconds[remote] = 0;
    t->next_probe = t->rounds + LMB_HYBRID_PROBE_INTERVAL;
    t->probe_cursor = remote % lmb_hybrid_remote_limit(t->top_k) + 1;
}
#endif
