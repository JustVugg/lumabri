/* Slow, exceptional numerical reference for the OLMoE f32/int8 expert.
 * Not an inference kernel: caller retains the original donor contribution
 * only after BOTH production outputs pass the unchanged error envelope.
 * Row-major q8 weights/scales must already be resident; no file/network IO. */
#ifndef LUMABRI_Q8_REFERENCE_H
#define LUMABRI_Q8_REFERENCE_H
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
_Static_assert(sizeof(long double) <= 16, "reference scratch budget must cover long double");
static inline int lmb_q8_expert_reference(const float *x, int dim, int inter,
    const int8_t *g, const int8_t *u, const int8_t *d,
    const float *gs, const float *us, const float *ds, float *out) {
    if (!x || !g || !u || !d || !gs || !us || !ds || !out ||
        dim < 1 || dim > 65536 || inter < 1 || inter > 65536) return -1;
    /* Fast-math can discard finite checks/reassociate the reference sums.
     * Such a build is not eligible for this recovery path. */
#if defined(__FAST_MATH__) || LDBL_MANT_DIG < 53
    return -1;
#endif
    for (int i = 0; i < dim; i++) if (!isfinite(x[i]) || !isfinite(ds[i])) return -1;
    for (int j = 0; j < inter; j++) if (!isfinite(gs[j]) || !isfinite(us[j])) return -1;
    long double *a = malloc((size_t)inter * sizeof *a);
    if (!a) return -1;
    int rc = -1;
    for (int j = 0; j < inter; j++) {
        long double gate = 0, up = 0;
        for (int i = 0; i < dim; i++) {
            gate += (long double)x[i] * g[(size_t)j * dim + i];
            up += (long double)x[i] * u[(size_t)j * dim + i];
        }
        gate *= gs[j]; up *= us[j];
        if (!isfinite(gate) || !isfinite(up)) goto done;
        /* Stable SiLU on both sides of zero (no exp(-large negative)). */
        long double e = expl(-fabsl(gate));
        a[j] = gate * (gate < 0 ? e / (1 + e) : 1 / (1 + e)) * up;
        if (!isfinite(a[j])) goto done;
    }
    for (int i = 0; i < dim; i++) {
        long double value = 0;
        for (int j = 0; j < inter; j++) value += a[j] * d[(size_t)i * inter + j];
        value *= ds[i];
        if (!isfinite(value) || fabsl(value) > FLT_MAX) goto done;
        out[i] = (float)value;
    }
    rc = 0;
done:
    free(a); return rc;
}
#endif
