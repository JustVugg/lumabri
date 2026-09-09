/* Resource advice, not model-quality rankings or synthetic speed estimates.
 * The producer must admit a candidate only after validating its actual plan. */
#ifndef LUMABRI_CATALOGUE_ADVICE_H
#define LUMABRI_CATALOGUE_ADVICE_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>

enum {
    LMB_ADVICE_LOWEST_RAM = 1u,
    LMB_ADVICE_LARGEST_CHECKPOINT = 2u,
    LMB_ADVICE_FASTEST_OBSERVED = 4u
};
typedef struct {
    int eligible;               /* verified resident plan on selected donors */
    uint64_t reserved_bytes;    /* aggregate process reservation, including Edge */
    uint64_t checkpoint_bytes;
    double measured_tok_s;      /* zero unless its current exact key matches */
} LmbAdviceCandidate;

static inline void lmb_catalogue_advice(const LmbAdviceCandidate *c, size_t n,
                                      uint32_t *flags) {
    if (!flags) return;
    for (size_t i = 0; i < n; i++) flags[i] = 0;
    if (!c) return;
    size_t least = n, largest = n, fastest = n, admitted = 0, measured = 0;
    for (size_t i = 0; i < n; i++) {
        if (!c[i].eligible || !c[i].reserved_bytes || !c[i].checkpoint_bytes ||
            c[i].reserved_bytes == UINT64_MAX || c[i].checkpoint_bytes == UINT64_MAX) continue;
        admitted++;
        if (least == n || c[i].reserved_bytes < c[least].reserved_bytes) least = i;
        if (largest == n || c[i].checkpoint_bytes > c[largest].checkpoint_bytes) largest = i;
        if (isfinite(c[i].measured_tok_s) && c[i].measured_tok_s > 0) {
            measured++;
            if (fastest == n || c[i].measured_tok_s > c[fastest].measured_tok_s) fastest = i;
        }
    }
    /* With only one option there is no meaningful comparative advice. */
    if (admitted > 1) {
        flags[least] |= LMB_ADVICE_LOWEST_RAM;
        flags[largest] |= LMB_ADVICE_LARGEST_CHECKPOINT;
    }
    if (measured > 1) flags[fastest] |= LMB_ADVICE_FASTEST_OBSERVED;
}

static inline const char *lmb_advice_text(uint32_t flags) {
    if (flags & LMB_ADVICE_FASTEST_OBSERVED) return "fastest last run";
    if (flags & LMB_ADVICE_LOWEST_RAM) return "lowest RAM reservation";
    if (flags & LMB_ADVICE_LARGEST_CHECKPOINT) return "largest resident checkpoint";
    return "";
}
#endif
