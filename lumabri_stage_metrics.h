/* Per-turn, per-range RUN observations. These are client-side elapsed times:
 * transport, serialization, queueing and remote execution, NOT kernel times.
 * No allocation, extra request, or extrapolation to an unmeasured range. */
#ifndef LUMABRI_STAGE_METRICS_H
#define LUMABRI_STAGE_METRICS_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define LMB_STAGE_PROFILE_MAX 32u
typedef struct {
    uint32_t begin, end, calls;
    double seconds;
} LmbStageSample;

/* Bounded STAT extension, before PERF1 so old rate readers still work.
 * These are observed RUN round trips, not predicted layer/kernel timings. */
static inline int lmb_stage_samples_parse(const char *stat, LmbStageSample *out,
                                         uint32_t *count) {
    if (!stat || !out || !count) return -1;
    *count = 0;
    const char *p = strstr(stat, " STAGES1 ");
    if (!p) return 1;
    p += 9;
    char *end; errno = 0;
    unsigned long n = strtoul(p, &end, 10);
    if (*p < '0' || *p > '9' || errno || *end != ' ' || !n || n > LMB_STAGE_PROFILE_MAX) return -1;
    p = end + 1;
    LmbStageSample samples[LMB_STAGE_PROFILE_MAX] = {{0}};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *fields[] = {&samples[i].begin, &samples[i].end, &samples[i].calls};
        for (unsigned j = 0; j < 3; j++) {
            errno = 0; unsigned long v = strtoul(p, &end, 10);
            if (*p < '0' || *p > '9' || errno || v > 1048576 || *end != ' ') return -1;
            *fields[j] = (uint32_t)v; p = end + 1;
        }
        errno = 0; samples[i].seconds = strtod(p, &end);
        if (*p < '0' || *p > '9' || errno || *end != ' ' ||
            !isfinite(samples[i].seconds) || samples[i].seconds <= 0 || samples[i].seconds > 1e9 ||
            !samples[i].calls || samples[i].begin >= samples[i].end ||
            samples[i].begin != (i ? samples[i-1].end : 0)) return -1;
        p = end + 1;
    }
    if (strncmp(p, "PERF1 ", 6)) return -1;
    memcpy(out, samples, n * sizeof *out); *count = (uint32_t)n;
    return 0;
}

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
