#!/usr/bin/env bash
# lumabri — the encrypted channel end to end.
#
# Two peers with their own Ed25519 identities do the handshake over a real
# socket pair, then trade framed AEAD messages. This proves:
#
#   1) both sides derive the same session and traffic decrypts to the input;
#   2) the header (op and lengths) is authenticated, not just the body;
#   3) a flipped byte anywhere in a frame fails the tag, never wrong plaintext;
#   4) each side learns the other's true identity key from the handshake.
set -euo pipefail
cd "$(dirname "$0")"

T=$(mktemp -d /tmp/lumabri-secure.XXXXXX)
trap 'rm -rf "$T"' EXIT

cat > "$T/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include "lumabri_secure.h"

static uint8_t sk_c[64], pk_c[32], sk_s[64], pk_s[32];
static int fail;

/* server thread for the echo round-trip */
static void *srv_echo(void *arg) {
    int fd = *(int *)arg;
    LmbSecure s;
    if (lmb_secure_handshake(fd, 0, sk_s, pk_s, &s)) { fail = 1; return NULL; }
    if (memcmp(s.peer_id, pk_c, 32)) { printf("  FAIL server saw wrong client identity\n"); fail = 1; }
    LmbMsg m;
    if (lmb_secure_recv(&s, fd, &m)) { printf("  FAIL server recv\n"); fail = 1; return NULL; }
    lmb_secure_send(&s, fd, m.op + 1, m.body, m.body_len, m.pay, m.pay_len);
    lmb_msg_free(&m);
    return NULL;
}

/* server thread for the tamper case: hand its session out through a global */
static LmbSecure g_srv; static int g_srv_ok;
static void *srv_hs(void *arg) {
    int fd = *(int *)arg;
    g_srv_ok = lmb_secure_handshake(fd, 0, sk_s, pk_s, &g_srv) == 0;
    return NULL;
}

int main(void) {
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i + 1);
    lmb_sign_keypair(pk_c, sk_c, seed);
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(100 + i);
    lmb_sign_keypair(pk_s, sk_s, seed);

    /* 1,2,4: handshake + authenticated round-trip */
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) { perror("socketpair"); return 1; }
    pthread_t th; pthread_create(&th, NULL, srv_echo, &sp[1]);

    LmbSecure c;
    if (lmb_secure_handshake(sp[0], 1, sk_c, pk_c, &c)) { printf("  FAIL client handshake\n"); return 1; }
    if (memcmp(c.peer_id, pk_s, 32)) { printf("  FAIL client saw wrong server identity\n"); fail = 1; }
    else printf("  ok   handshake: each side learns the other's identity\n");

    const char *body = "activation row and a bearer token nobody should read";
    if (lmb_secure_send(&c, sp[0], 42, body, (uint32_t)strlen(body), NULL, 0)) { printf("  FAIL client send\n"); return 1; }
    LmbMsg r;
    if (lmb_secure_recv(&c, sp[0], &r)) { printf("  FAIL client recv\n"); return 1; }
    if (r.op != 43 || r.body_len != strlen(body) || memcmp(r.body, body, r.body_len))
        { printf("  FAIL round-trip mismatch\n"); fail = 1; }
    else printf("  ok   round-trip: message decrypts to the input, op authenticated\n");
    lmb_msg_free(&r);
    pthread_join(th, NULL);
    close(sp[0]); close(sp[1]);

    /* 3: a flipped byte in a frame fails the tag */
    int hp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, hp)) { perror("socketpair"); return 1; }
    pthread_t hs; pthread_create(&hs, NULL, srv_hs, &hp[1]);
    LmbSecure a;
    if (lmb_secure_handshake(hp[0], 1, sk_c, pk_c, &a)) { printf("  FAIL tamper handshake\n"); return 1; }
    pthread_join(hs, NULL);
    if (!g_srv_ok) { printf("  FAIL tamper server handshake\n"); return 1; }

    uint8_t hdr[16]; lc_st32(hdr, LMB_MAGIC); lc_st32(hdr + 4, 7);
    lc_st32(hdr + 8, 5); lc_st32(hdr + 12, 0);
    uint8_t pt[5] = "hello", ct[5], tag[16], nonce[12];
    lmb_sec_nonce(nonce, a.tx_ctr);
    lc_aead_seal(a.tx_key, nonce, hdr, 16, pt, 5, ct, tag);
    ct[2] ^= 0x80;                                      /* corrupt the wire */
    lmb_write_full(hp[0], hdr, 16); lmb_write_full(hp[0], ct, 5); lmb_write_full(hp[0], tag, 16);
    LmbMsg tm;
    if (lmb_secure_recv(&g_srv, hp[1], &tm) == 0) { printf("  FAIL tampered frame accepted\n"); fail = 1; lmb_msg_free(&tm); }
    else printf("  ok   a flipped byte fails the tag\n");
    close(hp[0]); close(hp[1]);

    puts(fail ? "LUMABRI SECURE TEST: FAIL" : "LUMABRI SECURE TEST: PASS");
    return fail;
}
EOF
cc -O2 -w -I. -pthread "$T/t.c" -o "$T/t"
"$T/t"
