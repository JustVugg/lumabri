#include "lumabri_sampling.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The plain definition: weigh every token, sort the whole vocabulary, keep
 * the shortest prefix that holds top_p of the mass. The sampler must draw
 * the same token from the same state without sorting everything. */
typedef struct { double weight; uint32_t token; } Reference;

static int reference_desc(const void *left, const void *right) {
    const Reference *a = left, *b = right;
    if (a->weight > b->weight) return -1;
    if (a->weight < b->weight) return 1;
    return a->token < b->token ? -1 : a->token != b->token;
}

static uint64_t reference_next(uint64_t *state) {
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int32_t reference_sample(uint64_t *state, const float *logits, uint32_t n,
                                double temperature, double top_p) {
    float maximum = -INFINITY;
    for (uint32_t i = 0; i < n; i++)
        if (isfinite(logits[i]) && logits[i] > maximum) maximum = logits[i];
    Reference *all = malloc((size_t)n * sizeof *all);
    assert(all);
    size_t count = 0, kept; double total = 0, kept_total;
    for (uint32_t i = 0; i < n; i++) {
        if (!isfinite(logits[i])) continue;
        double weight = exp(((double)logits[i] - maximum) / temperature);
        if (!(weight > 0.0) || !isfinite(weight)) continue;
        all[count++] = (Reference){weight, i}; total += weight;
    }
    assert(count);
    kept = count; kept_total = total;
    if (top_p < 1.0) {
        qsort(all, count, sizeof *all, reference_desc);
        kept = 0; kept_total = 0;
        do { kept_total += all[kept].weight; kept++; } while (kept < count && kept_total < top_p * total);
    }
    double draw = ((double)(reference_next(state) >> 11) + 0.5) * (1.0 / 9007199254740992.0) * kept_total;
    int32_t token = (int32_t)all[kept - 1].token;
    for (size_t i = 0; i < kept; i++) {
        if (draw < all[i].weight) { token = (int32_t)all[i].token; break; }
        draw -= all[i].weight;
    }
    free(all);
    return token;
}

static void same_as_reference(void) {
    enum { VOCAB = 60000 };
    float *logits = malloc(VOCAB * sizeof *logits);
    assert(logits);
    const double temperatures[] = {0.05, 0.7, 1.0, 4.0, 60.0};
    const double nuclei[] = {0.01, 0.5, 0.95, 0.999999, 1.0};
    uint64_t noise = 12345;
    for (int shape = 0; shape < 6; shape++) {
        /* Peaked, ordinary, flat, heavily tied, a single finite token, and a
         * spread wide enough for most weights to underflow. */
        double spread = shape == 0 ? 40.0 : shape == 1 ? 8.0 : shape == 2 ? 0.01 : shape == 5 ? 3000.0 : 6.0;
        for (uint32_t i = 0; i < VOCAB; i++) {
            double unit = (double)(reference_next(&noise) >> 11) / 9007199254740992.0;
            logits[i] = (float)(spread * unit);
            if (shape == 3) logits[i] = (float)(int)(logits[i]);
            if (shape == 4) logits[i] = i == 777 ? 1.5f : -INFINITY;
            if (i % 997 == 0 && shape != 4) logits[i] = i % 2 ? -INFINITY : NAN;
        }
        for (unsigned t = 0; t < sizeof temperatures / sizeof *temperatures; t++)
            for (unsigned p = 0; p < sizeof nuclei / sizeof *nuclei; p++) {
                LmbSampler sampler;
                lmb_sampler_init(&sampler, 1000 + shape);
                uint64_t state = sampler.state;
                for (int draw = 0; draw < 5; draw++) {
                    int32_t got = -1;
                    assert(!lmb_sample_logits(&sampler, logits, VOCAB, temperatures[t], nuclei[p], &got));
                    assert(got == reference_sample(&state, logits, VOCAB, temperatures[t], nuclei[p]));
                    assert(sampler.state == state);
                }
            }
    }
    free(logits);
}

int main(void) {
    LmbSampler first, second;
    lmb_sampler_init(&first, 42);
    lmb_sampler_init(&second, 42);
    const float logits[] = {0.0f, 1.0f, 4.0f, 2.0f};
    for (int i = 0; i < 100; i++) {
        int32_t a = -1, b = -1;
        assert(!lmb_sample_logits(&first, logits, 4, 0.7, 0.95, &a));
        assert(!lmb_sample_logits(&second, logits, 4, 0.7, 0.95, &b));
        assert(a == b && a >= 0 && a < 4);
    }

    /* A sufficiently tight nucleus contains only the maximum token. */
    lmb_sampler_init(&first, 7);
    for (int i = 0; i < 100; i++) {
        int32_t selected = -1;
        assert(!lmb_sample_logits(&first, logits, 4, 1.0, 0.5, &selected));
        assert(selected == 2);
    }

    float infinities[] = {-INFINITY, INFINITY, 1.0f, INFINITY};
    int seen_one = 0, seen_three = 0;
    lmb_sampler_init(&first, 99);
    for (int i = 0; i < 100; i++) {
        int32_t selected = -1;
        assert(!lmb_sample_logits(&first, infinities, 4, 1.0, 1.0,
                                  &selected));
        assert(selected == 1 || selected == 3);
        seen_one |= selected == 1;
        seen_three |= selected == 3;
    }
    assert(seen_one && seen_three);

    int32_t selected = -1;
    assert(lmb_sample_logits(&first, logits, 4, 0.0, 1.0, &selected));
    assert(lmb_sample_logits(&first, logits, 4, 1.0, 0.0, &selected));
    same_as_reference();
    puts("sampling tests: ok");
    return 0;
}
