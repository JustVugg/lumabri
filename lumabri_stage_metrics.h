/* Per-turn, per-range RUN observations. These are client-side elapsed times:
 * transport, serialization, queueing and remote execution, NOT kernel times.
 * No allocation, extra request, or extrapolation to an unmeasured range. */
#ifndef LUMABRI_STAGE_METRICS_H
#define LUMABRI_STAGE_METRICS_H
#include <math.h>
#include <stdint.h>

typedef struct {
    uint64_t prefill_calls, prefill_rows, decode_calls;
    double prefill_seconds, decode_seconds, decode_min_seconds, decode_max_seconds;
} LmbStageMetrics;

/* Failure leaves the previous observation intact. The caller must mark the
 * complete turn profile unavailable, never publish a partially valid chain. */
static inline int lmb_stage_metrics_add(LmbStageMetrics *m, int decode,
                                       uint32_t rows, double seconds) {
    if (!m || (decode != 0 && decode != 1) || !rows || (decode && rows != 1) ||
        !isfinite(seconds) || seconds < 0) return -1;
    LmbStageMetrics next = *m;
    if (decode) {
        if (next.decode_calls == UINT64_MAX) return -1;
        if (!next.decode_calls || seconds < next.decode_min_seconds)
            next.decode_min_seconds = seconds;
        if (seconds > next.decode_max_seconds) next.decode_max_seconds = seconds;
        next.decode_calls++;
        next.decode_seconds += seconds;
    } else {
        if (next.prefill_calls == UINT64_MAX || next.prefill_rows > UINT64_MAX - rows)
            return -1;
        next.prefill_calls++;
        next.prefill_rows += rows;
        next.prefill_seconds += seconds;
    }
    if (!isfinite(next.prefill_seconds) || !isfinite(next.decode_seconds)) return -1;
    *m = next;
    return 0;
}
#endif
