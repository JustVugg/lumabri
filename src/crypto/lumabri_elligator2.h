/* lumabri_elligator2.h — hashing a field element onto Curve25519.
 *
 * The pairing exchange (src/crypto/lumabri_cpace.h) needs a point whose
 * discrete logarithm nobody knows, derived from a short code. That is the
 * Elligator 2 map: every field element r is sent to the u-coordinate of a
 * point of the Montgomery curve v^2 = u^3 + A u^2 + u over GF(2^255-19),
 * A = 486662, following the formulas of RFC 9380 section 6.7.1 with the
 * non-square Z = 2. Only u is produced, because X25519 never uses v.
 *
 * This file adds the three field operations the existing arithmetic in
 * lumabri_sign_core.h does not have (zero test, square test, selection) and
 * the map itself. Like that arithmetic, nothing here branches on, or indexes
 * memory by, a value derived from its input: the same sequence of field
 * multiplications runs for every r.
 */
#ifndef LUMABRI_ELLIGATOR2_H
#define LUMABRI_ELLIGATOR2_H

#include "src/crypto/lumabri_sign_core.h"

/* 1 when a is zero modulo p, 0 otherwise. Constant time. */
static LMB_MAYBE_UNUSED int lmb_fe_is_zero(const lmb_gf a) {
    uint8_t s[32];
    uint32_t d = 0;
    lmb_pack25519(s, a);                 /* the canonical representative */
    for (int i = 0; i < 32; i++) d |= s[i];
    return (int)(1 & ((d - 1) >> 8));
}

/* r = b ? x : y, with b exactly 0 or 1. Constant time. */
static LMB_MAYBE_UNUSED void lmb_fe_select(lmb_gf r, const lmb_gf x, const lmb_gf y, int b) {
    int64_t mask = -(int64_t)(b & 1);
    for (int i = 0; i < 16; i++) r[i] = y[i] ^ (mask & (x[i] ^ y[i]));
}

/* Euler's criterion: a^((p-1)/2) is 0 for zero, 1 for a non-zero square and
 * p-1 otherwise. (p-1)/2 = 2^254 - 10 = 4 * (2^252 - 3) + 2, so the existing
 * exponentiation by 2^252 - 3 does almost all of the work. */
static LMB_MAYBE_UNUSED void lmb_fe_legendre(lmb_gf o, const lmb_gf a) {
    lmb_gf t, a2;
    lmb_pow2523(t, a);
    lmb_S(t, t); lmb_S(t, t);
    lmb_S(a2, a);
    lmb_M(o, t, a2);
}

/* 1 when a is a square modulo p (zero counts as a square, as in RFC 9380),
 * 0 otherwise. Constant time. */
static LMB_MAYBE_UNUSED int lmb_fe_is_square(const lmb_gf a) {
    lmb_gf l, minus_one;
    lmb_fe_legendre(l, a);
    lmb_Z(minus_one, lmb_gf0, lmb_gf1);
    return lmb_neq(l, minus_one) & 1;        /* lmb_neq is 0 or -1 */
}

/* Elligator 2. `r` is 32 little-endian bytes; bit 255 is ignored and values
 * of p or more are reduced, exactly as X25519 treats a u-coordinate. `u`
 * receives the canonical u-coordinate of a point on the curve itself (never
 * on its twist).
 *
 *   x1  = -A / (1 + 2 r^2)            (and -A if that were 0)
 *   gx1 = x1^3 + A x1^2 + x1
 *   u   = x1          if gx1 is a square
 *         -x1 - A     otherwise
 *
 * 1 + 2 r^2 is never zero in this field (-1/2 is not a square), so the
 * exceptional case cannot occur; it is still handled, without a branch, so
 * that the code states the complete map. */
static LMB_MAYBE_UNUSED void lmb_elligator2(uint8_t u[32], const uint8_t r[32]) {
    static const lmb_gf A = { 0x6d06, 7 };            /* 486662 */
    lmb_gf e, t, x1, x2, gx1, minus_a, picked;
    lmb_unpack25519(e, r);
    lmb_S(t, e);
    lmb_A(t, t, t);                                   /* 2 r^2 */
    lmb_A(t, t, lmb_gf1);                             /* 1 + 2 r^2 */
    lmb_inv25519(t, t);                               /* inverse; 0 stays 0 */
    lmb_M(x1, A, t);
    lmb_Z(x1, lmb_gf0, x1);                           /* -A / (1 + 2 r^2) */
    lmb_Z(minus_a, lmb_gf0, A);
    lmb_fe_select(x1, minus_a, x1, lmb_fe_is_zero(x1));
    lmb_A(t, x1, A);
    lmb_M(t, t, x1);
    lmb_A(t, t, lmb_gf1);                             /* x1^2 + A x1 + 1 */
    lmb_M(gx1, t, x1);
    lmb_Z(x2, minus_a, x1);                           /* -x1 - A */
    lmb_fe_select(picked, x1, x2, lmb_fe_is_square(gx1));
    lmb_pack25519(u, picked);
}

#endif /* LUMABRI_ELLIGATOR2_H */
