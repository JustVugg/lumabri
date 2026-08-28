#include "lumabri_sampling.h"

#include <math.h>
#include <stdlib.h>

typedef struct {
    double weight;
    uint32_t token;
} LmbSampleCandidate;

static uint64_t splitmix64(uint64_t *state) {
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static double uniform_open(LmbSampler *sampler) {
    /* The midpoint keeps the draw strictly inside (0, 1). */
    return ((double)(splitmix64(&sampler->state) >> 11) + 0.5) *
           (1.0 / 9007199254740992.0);
}

void lmb_sampler_init(LmbSampler *sampler, uint64_t seed) {
    if (sampler)
        sampler->state = seed ^ UINT64_C(0x6a09e667f3bcc909);
}

static int candidate_desc(const void *left, const void *right) {
    const LmbSampleCandidate *a = left;
    const LmbSampleCandidate *b = right;
    if (a->weight > b->weight) return -1;
    if (a->weight < b->weight) return 1;
    return a->token < b->token ? -1 : a->token != b->token;
}

int lmb_sample_logits(LmbSampler *sampler, const float *logits,
                      uint32_t vocab_size, double temperature, double top_p,
                      int32_t *token) {
    if (!sampler || !logits || !vocab_size || !token ||
        !isfinite(temperature) || temperature <= 0.0 ||
        !isfinite(top_p) || top_p <= 0.0 || top_p > 1.0)
        return -1;

    uint32_t positive_infinities = 0;
    for (uint32_t i = 0; i < vocab_size; i++)
        if (isinf(logits[i]) && logits[i] > 0.0f) positive_infinities++;
    if (positive_infinities) {
        uint32_t wanted = (uint32_t)(uniform_open(sampler) *
                                     positive_infinities);
        if (wanted >= positive_infinities) wanted = positive_infinities - 1;
        for (uint32_t i = 0; i < vocab_size; i++)
            if (isinf(logits[i]) && logits[i] > 0.0f && !wanted--) {
                *token = (int32_t)i;
                return 0;
            }
        return -1;
    }

    float maximum = -INFINITY;
    for (uint32_t i = 0; i < vocab_size; i++)
        if (isfinite(logits[i]) && logits[i] > maximum) maximum = logits[i];
    if (!isfinite(maximum)) return -1;

    LmbSampleCandidate *candidates =
        malloc((size_t)vocab_size * sizeof(*candidates));
    if (!candidates) return -1;
    size_t count = 0;
    double total = 0.0;
    for (uint32_t i = 0; i < vocab_size; i++) {
        if (!isfinite(logits[i])) continue;
        double weight = exp(((double)logits[i] - maximum) / temperature);
        if (!(weight > 0.0) || !isfinite(weight)) continue;
        candidates[count++] = (LmbSampleCandidate){weight, i};
        total += weight;
    }
    if (!count || !(total > 0.0) || !isfinite(total)) {
        free(candidates);
        return -1;
    }

    size_t kept = count;
    double kept_total = total;
    if (top_p < 1.0) {
        qsort(candidates, count, sizeof(*candidates), candidate_desc);
        double target = top_p * total;
        kept_total = 0.0;
        kept = 0;
        do {
            kept_total += candidates[kept].weight;
            kept++;
        } while (kept < count && kept_total < target);
    }

    double draw = uniform_open(sampler) * kept_total;
    for (size_t i = 0; i < kept; i++) {
        if (draw < candidates[i].weight) {
            *token = (int32_t)candidates[i].token;
            free(candidates);
            return 0;
        }
        draw -= candidates[i].weight;
    }
    *token = (int32_t)candidates[kept - 1].token;
    free(candidates);
    return 0;
}
