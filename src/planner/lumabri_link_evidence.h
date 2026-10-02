/* Small, endpoint-bound observations, not NIC speed or kernel timing.
 * Probe traffic is bounded to three empty echoes and one 64 KiB echo.
 * The effective rate includes both directions, framing and encryption. */
#ifndef LUMABRI_LINK_EVIDENCE_H
#define LUMABRI_LINK_EVIDENCE_H
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define LMB_LINK_BYTES (64u << 10)
#define LMB_LINK_SAMPLES 3u
#define LMB_LINK_TTL_SECONDS 300.0
typedef struct {
    uint32_t samples;
    double rtt_p50_seconds, rtt_max_seconds, echo_bytes_per_second, measured_at;
} LmbLinkEvidence;

static inline int lmb_link_valid(const LmbLinkEvidence *v) {
    return v && v->samples == LMB_LINK_SAMPLES &&
        isfinite(v->rtt_p50_seconds) && v->rtt_p50_seconds > 0 &&
        isfinite(v->rtt_max_seconds) && v->rtt_max_seconds >= v->rtt_p50_seconds &&
        v->rtt_max_seconds < 60 && isfinite(v->echo_bytes_per_second) &&
        v->echo_bytes_per_second > 0 && v->echo_bytes_per_second <= 1e15 &&
        isfinite(v->measured_at) && v->measured_at > 0;
}
static inline int lmb_link_current(const LmbLinkEvidence *v, double now) {
    return lmb_link_valid(v) && isfinite(now) && now >= v->measured_at &&
        now - v->measured_at <= LMB_LINK_TTL_SECONDS;
}
static inline int lmb_link_observed(LmbLinkEvidence *v, const double times[3],
                                    double echo_seconds, double at) {
    double sorted[3]; memcpy(sorted, times, sizeof sorted);
    for (unsigned i = 0; i < 3; i++) {
        if (!isfinite(sorted[i]) || sorted[i] <= 0) return -1;
        for (unsigned j = i; j && sorted[j] < sorted[j-1]; j--) {
            double x = sorted[j]; sorted[j] = sorted[j-1]; sorted[j-1] = x;
        }
    }
    if (!isfinite(echo_seconds) || echo_seconds <= 0) return -1;
    LmbLinkEvidence next = {LMB_LINK_SAMPLES, sorted[1], sorted[2],
        2.0 * LMB_LINK_BYTES / echo_seconds, at};
    if (!lmb_link_valid(&next)) return -1;
    *v = next; return 0;
}

/* Optional STAT extension before STAGES1/PERF1. No partial chain is usable. */
static inline int lmb_links_parse(const char *stat, uint32_t n,
    const uint32_t *begins, const uint32_t *ends, LmbLinkEvidence *out) {
    const char *p = stat ? strstr(stat, " LINKS1 ") : NULL;
    if (!p || !n || n > 32) return -1;
    p += 8; char *end;
    errno = 0; unsigned long count = strtoul(p, &end, 10);
    if (*p < '0' || *p > '9' || errno || *end != ' ' || count != n) return -1;
    p = end + 1;
    LmbLinkEvidence links[32] = {{0}};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t expected[] = {begins[i], ends[i], LMB_LINK_SAMPLES};
        for (unsigned j = 0; j < 3; j++) {
            errno = 0; unsigned long x = strtoul(p, &end, 10);
            if (*p < '0' || *p > '9' || errno || *end != ' ' || x != expected[j]) return -1;
            p = end + 1;
        }
        links[i].samples = LMB_LINK_SAMPLES;
        double *fields[] = {&links[i].rtt_p50_seconds, &links[i].rtt_max_seconds,
            &links[i].echo_bytes_per_second, &links[i].measured_at};
        for (unsigned j = 0; j < 4; j++) {
            errno = 0; *fields[j] = strtod(p, &end);
            if (*p < '0' || *p > '9' || errno || *end != ' ') return -1;
            p = end + 1;
        }
        if (!lmb_link_valid(&links[i])) return -1;
    }
    if (strncmp(p, "STAGES1 ", 8) && strncmp(p, "PERF1 ", 6)) return -1;
    memcpy(out, links, n * sizeof *out); return 0;
}
#endif
