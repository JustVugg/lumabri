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

#define LMB_SAMPLE_BANDS 5

/* The weight of one token, or 0 when it cannot be drawn. */
static int sample_weight(float logit, float maximum, double temperature,
                         double *weight) {
    if (!isfinite(logit)) return 0;
    *weight = exp(((double)logit - maximum) / temperature);
    return *weight > 0.0 && isfinite(*weight);
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

    /* One pass gives the total and, per weight band, how many tokens and how
     * much mass lie at or above it. The heaviest token weighs exactly 1. */
    static const double band[LMB_SAMPLE_BANDS] = {
        0x1p-10, 0x1p-20, 0x1p-30, 0x1p-40, 0x1p-52};
    size_t band_count[LMB_SAMPLE_BANDS + 1] = {0}, count = 0;
    double band_total[LMB_SAMPLE_BANDS + 1] = {0}, total = 0.0;
    for (uint32_t i = 0; i < vocab_size; i++) {
        double weight;
        if (!sample_weight(logits[i], maximum, temperature, &weight)) continue;
        unsigned at = 0;
        while (at < LMB_SAMPLE_BANDS && weight < band[at]) at++;
        band_count[at]++; band_total[at] += weight;
        count++; total += weight;
    }
    if (!count || !(total > 0.0) || !isfinite(total)) return -1;

    if (top_p >= 1.0) {
        /* No cutoff: walk the tokens in vocabulary order. */
        double draw = uniform_open(sampler) * total;
        uint32_t last = 0;
        for (uint32_t i = 0; i < vocab_size; i++) {
            double weight;
            if (!sample_weight(logits[i], maximum, temperature, &weight)) continue;
            if (draw < weight) { *token = (int32_t)i; return 0; }
            draw -= weight; last = i;
        }
        *token = (int32_t)last;
        return 0;
    }

    /* The nucleus is a prefix of the tokens ordered by weight. Sorting the
     * whole vocabulary to find a prefix of a few tokens costs more than the
     * head projection itself, so sort only the heaviest bands that already
     * hold the target mass. Every token outside them is lighter than every
     * token inside, so the prefix is the same one a full sort would give.
     * The margin covers the different summation order of the two totals. */
    double target = top_p * total, floor = 0.0, held = 0.0;
    size_t wanted = 0;
    for (unsigned at = 0; at < LMB_SAMPLE_BANDS; at++) {
        wanted += band_count[at]; held += band_total[at];
        if (held >= target * (1.0 + 1e-9)) { floor = band[at]; break; }
    }
    if (!(floor > 0.0)) wanted = count;
    LmbSampleCandidate *candidates = malloc(wanted * sizeof(*candidates));
    if (!candidates) return -1;
    /* A logit this far below the floor cannot reach it: skip its exp(). */
    double skip = floor > 0.0 ? (double)maximum + temperature * log(floor) -
                                1e-3 * (1.0 + fabs((double)maximum)) : -INFINITY;
    size_t found = 0;
    for (uint32_t i = 0; i < vocab_size && found < wanted; i++) {
        double weight;
        if ((double)logits[i] < skip ||
            !sample_weight(logits[i], maximum, temperature, &weight) ||
            weight < floor) continue;
        candidates[found++] = (LmbSampleCandidate){weight, i};
    }
    if (found != wanted) { free(candidates); return -1; }
    qsort(candidates, found, sizeof(*candidates), candidate_desc);
    double kept_total = 0.0;
    size_t kept = 0;
    do {
        kept_total += candidates[kept].weight;
        kept++;
    } while (kept < found && kept_total < target);

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
