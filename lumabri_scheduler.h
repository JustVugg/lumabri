#ifndef LUMABRI_SCHEDULER_H
#define LUMABRI_SCHEDULER_H

#include <stdint.h>

/* Small, allocation-free latency model shared by Expert and Segment routing.
 * Times are microseconds. The p95 estimate deliberately falls slowly: a tail
 * spike should influence hedging longer than one fast reply influences EWMA. */
typedef struct {
    uint64_t ewma_us;
    uint64_t p95_us;
    uint64_t circuit_until_ms;
    uint32_t samples;
    uint32_t consecutive_failures;
} LmbLatencyPredictor;

static inline void lmb_predict_init(LmbLatencyPredictor *p, uint64_t initial_us) {
    if (!initial_us) initial_us = 1000;
    p->ewma_us = p->p95_us = initial_us;
    p->circuit_until_ms = 0;
    p->samples = p->consecutive_failures = 0;
}

static inline void lmb_predict_observe(LmbLatencyPredictor *p, uint64_t sample_us) {
    if (!sample_us) sample_us = 1;
    if (!p->ewma_us) lmb_predict_init(p, sample_us);
    p->ewma_us = (p->ewma_us * 7 + sample_us) / 8;
    if (sample_us > p->p95_us)
        p->p95_us = (p->p95_us * 7 + sample_us) / 8;
    else
        p->p95_us = (p->p95_us * 31 + sample_us) / 32;
    p->samples++;
    p->consecutive_failures = 0;
    p->circuit_until_ms = 0;
}

static inline void lmb_predict_failure(LmbLatencyPredictor *p, uint64_t now_ms) {
    if (!p->ewma_us) lmb_predict_init(p, 1000);
    if (p->consecutive_failures < 31) p->consecutive_failures++;
    if (p->consecutive_failures >= 3) {
        uint32_t shift = p->consecutive_failures - 3;
        uint64_t cooldown = 1000ull << (shift > 5 ? 5 : shift);
        p->circuit_until_ms = now_ms + cooldown;
    }
}

static inline int lmb_predict_available(const LmbLatencyPredictor *p,
                                        uint64_t now_ms) {
    return !p->circuit_until_ms || now_ms >= p->circuit_until_ms;
}

static inline uint64_t lmb_predict_score(const LmbLatencyPredictor *p,
                                         uint32_t queue_ahead) {
    uint64_t base = p->ewma_us ? p->ewma_us : 1000;
    uint64_t score = base * ((uint64_t)queue_ahead + 1);
    score += (uint64_t)p->consecutive_failures * base;
    return score;
}

/* Zero means do not hedge. A user override is handled by the caller. */
static inline uint32_t lmb_predict_hedge_ms(const LmbLatencyPredictor *p) {
    if (p->samples < 8 || p->p95_us <= p->ewma_us + p->ewma_us / 2) return 0;
    uint64_t delay = (p->ewma_us + p->p95_us) / 2000;
    if (delay < 2) delay = 2;
    if (delay > 250) delay = 250;
    return (uint32_t)delay;
}

#endif
