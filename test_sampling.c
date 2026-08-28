#include "lumabri_sampling.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

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
    puts("sampling tests: ok");
    return 0;
}
