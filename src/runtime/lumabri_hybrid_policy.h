/* Per-layer measured scheduling. All-local and split use the SAME expert
 * callback and router accumulation order. This is not a speed guarantee:
 * probes cost work, load varies, and the decision must remain reversible. */
#ifndef LUMABRI_HYBRID_POLICY_H
#define LUMABRI_HYBRID_POLICY_H
#include <stdint.h>
#include <math.h>
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
