#ifndef LUMABRI_SAMPLING_H
#define LUMABRI_SAMPLING_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t state;
} LmbSampler;

/* A zero seed is accepted and expanded to a non-zero deterministic state. */
void lmb_sampler_init(LmbSampler *sampler, uint64_t seed);

/* Exact categorical sampling after temperature scaling and nucleus cutoff.
 * temperature must be positive and top_p must be in (0, 1]. */
int lmb_sample_logits(LmbSampler *sampler, const float *logits,
                      uint32_t vocab_size, double temperature, double top_p,
                      int32_t *token);

#endif
