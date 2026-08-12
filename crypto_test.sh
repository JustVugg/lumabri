#!/usr/bin/env bash
# lumabri — the transport primitives against their published test vectors.
#
# ChaCha20, Poly1305 and the AEAD from RFC 8439, and X25519 from RFC 7748.
# A cipher that merely round-trips proves nothing; a cipher that reproduces
# the RFC's own bytes is the real check, the same standard sign_test holds
# Ed25519 to. Also a tamper case: one flipped byte must fail the tag.
set -euo pipefail
cd "$(dirname "$0")"

T=$(mktemp -d /tmp/lumabri-crypto.XXXXXX)
trap 'rm -rf "$T"' EXIT

cat > "$T/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "lumabri_crypto.h"

static int fail;
static void hx(const char *label, const uint8_t *got, const char *want, size_t n) {
    char h[256]; for (size_t i = 0; i < n; i++) sprintf(h + 2*i, "%02x", got[i]);
    if (strcmp(h, want)) { printf("  FAIL %s\n    got  %s\n    want %s\n", label, h, want); fail = 1; }
    else printf("  ok   %s\n", label);
}
static void unhex(uint8_t *d, const char *s) {
    lmb_unhex(d, s, strlen(s) / 2);
}

int main(void) {
    /* RFC 8439 §2.4.2 — ChaCha20 encryption */
    {
        uint8_t key[32], nonce[12], out[114];
        unhex(key, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        memset(nonce, 0, 12); nonce[7] = 0x4a; /* 00:00:00:00:00:00:00:4a:00:00:00:00 */
        const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                         "only one tip for the future, sunscreen would be it.";
        lc_chacha20(key, 1, nonce, (const uint8_t*)pt, out, strlen(pt));
        hx("ChaCha20 (RFC 8439 2.4.2)", out,
           "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
           "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
           "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
           "5af90bbf74a35be6b40b8eedf2785e42874d", 114);
    }
    /* RFC 8439 §2.5.2 — Poly1305 */
    {
        uint8_t key[32], mac[16];
        unhex(key, "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
        const char *m = "Cryptographic Forum Research Group";
        lc_poly st; lc_poly_init(&st, key);
        lc_poly_update(&st, (const uint8_t*)m, strlen(m));
        lc_poly_finish(&st, mac);
        hx("Poly1305 (RFC 8439 2.5.2)", mac, "a8061dc1305136c6c22b8baf0c0127a9", 16);
    }
    /* RFC 8439 §2.8.2 — AEAD seal */
    {
        uint8_t key[32], nonce[12], aad[12], ct[114], tag[16];
        unhex(key, "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        unhex(nonce, "070000004041424344454647");
        unhex(aad, "50515253c0c1c2c3c4c5c6c7");
        const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                         "only one tip for the future, sunscreen would be it.";
        lc_aead_seal(key, nonce, aad, 12, (const uint8_t*)pt, strlen(pt), ct, tag);
        hx("AEAD ct (RFC 8439 2.8.2)", ct,
           "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
           "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
           "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
           "3ff4def08e4b7a9de576d26586cec64b6116", 114);
        hx("AEAD tag (RFC 8439 2.8.2)", tag, "1ae10b594f09e26a7e902ecbd0600691", 16);
        /* open round-trips, and one flipped byte fails the tag */
        uint8_t dec[114];
        if (lc_aead_open(key, nonce, aad, 12, ct, strlen(pt), tag, dec) ||
            memcmp(dec, pt, strlen(pt))) { printf("  FAIL AEAD open round-trip\n"); fail = 1; }
        else printf("  ok   AEAD open round-trip\n");
        ct[0] ^= 1;
        if (lc_aead_open(key, nonce, aad, 12, ct, strlen(pt), tag, dec) == 0) {
            printf("  FAIL AEAD accepted a tampered ciphertext\n"); fail = 1;
        } else printf("  ok   AEAD rejects a flipped byte\n");
    }
    /* RFC 7748 §5.2 — X25519 */
    {
        uint8_t s[32], u[32], out[32];
        unhex(s, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
        unhex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
        lc_x25519(out, s, u);
        hx("X25519 (RFC 7748 5.2)", out,
           "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);
    }
    /* X25519 Diffie-Hellman agreement: both sides reach the same secret */
    {
        uint8_t a[32], b[32], A[32], B[32], sa[32], sb[32];
        for (int i = 0; i < 32; i++) { a[i] = (uint8_t)(i + 1); b[i] = (uint8_t)(200 - i); }
        lc_x25519_base(A, a); lc_x25519_base(B, b);
        lc_x25519(sa, a, B); lc_x25519(sb, b, A);
        if (memcmp(sa, sb, 32)) { printf("  FAIL X25519 DH disagreement\n"); fail = 1; }
        else printf("  ok   X25519 DH agreement\n");
    }
    puts(fail ? "LUMABRI CRYPTO TEST: FAIL" : "LUMABRI CRYPTO TEST: PASS");
    return fail;
}
EOF
cc -O2 -w -I. "$T/t.c" -o "$T/t"
"$T/t"
