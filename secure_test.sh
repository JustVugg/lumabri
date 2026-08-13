#!/usr/bin/env bash
# lumabri — the encrypted channel end to end.
#
# Two peers with their own Ed25519 identities do the handshake over a real
# socket pair, then trade framed AEAD messages. This proves:
#
#   1) both sides derive the same session and traffic decrypts to the input;
#   2) the header (op and lengths) is authenticated, not just the body;
#   3) ciphertext and header tampering both fail authentication;
#   4) replay and a low-order X25519 point are rejected;
#   5) persistent TOFU rejects an endpoint key change;
#   6) closing an encrypted fd removes and wipes its registered session.
set -euo pipefail
cd "$(dirname "$0")"

T=$(mktemp -d /tmp/lumabri-secure.XXXXXX)
trap 'rm -rf "$T"' EXIT
export HOME="$T"

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
    if (lmb_secure_handshake(fd, 0, sk_s, pk_s, &s)) { perror("  FAIL server handshake"); fail = 1; return NULL; }
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

static void *srv_low_order(void *arg) {
    int fd = *(int *)arg; uint8_t peer[32], zero[32] = {0};
    if (lmb_read_full(fd, peer, sizeof peer) || lmb_write_full(fd, zero, sizeof zero))
        fail = 1;
    return NULL;
}

static int secure_pair(int sp[2], LmbSecure *client, LmbSecure *server) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) return -1;
    pthread_t hs; g_srv_ok = 0;
    pthread_create(&hs, NULL, srv_hs, &sp[1]);
    int rc = lmb_secure_handshake(sp[0], 1, sk_c, pk_c, client);
    pthread_join(hs, NULL);
    if (rc || !g_srv_ok) return -1;
    *server = g_srv;
    return 0;
}

static int registered(int fd) {
    if (fd < 0 || fd >= LMB_SEC_MAXFD) return 0;
    pthread_mutex_lock(&g_sec_lk);
    int yes = g_sec_reg[fd] && !g_sec_reg[fd]->closing;
    pthread_mutex_unlock(&g_sec_lk);
    return yes;
}

static void raw_frame(const LmbSecure *s, int fd, uint64_t ctr,
                      uint8_t op, const uint8_t *pt, uint32_t n,
                      int flip_header, int flip_cipher) {
    uint8_t hdr[16], tag[16], nonce[12], *ct = malloc(n ? n : 1);
    lc_st32(hdr, LMB_MAGIC); lc_st32(hdr + 4, op);
    lc_st32(hdr + 8, n); lc_st32(hdr + 12, 0);
    lmb_sec_nonce(nonce, ctr);
    lc_aead_seal(s->tx_key, nonce, hdr, 16, pt, n, ct, tag);
    if (flip_header) hdr[4] ^= 1;
    if (flip_cipher && n) ct[n / 2] ^= 0x80;
    lmb_write_full(fd, hdr, 16); lmb_write_full(fd, ct, n); lmb_write_full(fd, tag, 16);
    free(ct);
}

typedef struct { int fd; uint8_t *want; } WrapArg;
static void *srv_wrap(void *arg) {
    WrapArg *wa = (WrapArg *)arg;
    if (lmb_secure_server(wa->fd) ||
        !lmb_secure_peer_matches(wa->fd, wa->want)) fail = 1;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
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
    if (lmb_secure_handshake(sp[0], 1, sk_c, pk_c, &c)) { perror("  FAIL client handshake"); return 1; }
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

    /* 3a: a flipped ciphertext byte fails the tag */
    int hp[2];
    LmbSecure a, z;
    if (secure_pair(hp, &a, &z)) { printf("  FAIL tamper handshake\n"); return 1; }
    const uint8_t pt[5] = "hello";
    raw_frame(&a, hp[0], 0, 7, pt, 5, 0, 1);
    LmbMsg tm;
    if (lmb_secure_recv(&z, hp[1], &tm) == 0) { printf("  FAIL tampered ciphertext accepted\n"); fail = 1; lmb_msg_free(&tm); }
    else printf("  ok   ciphertext tampering fails the tag\n");
    close(hp[0]); close(hp[1]);

    /* 3b: the header is AAD, so changing op also fails the tag. */
    if (secure_pair(hp, &a, &z)) return 1;
    raw_frame(&a, hp[0], 0, 7, pt, 5, 1, 0);
    if (lmb_secure_recv(&z, hp[1], &tm) == 0) { printf("  FAIL tampered header accepted\n"); fail = 1; lmb_msg_free(&tm); }
    else printf("  ok   header tampering fails the tag\n");
    close(hp[0]); close(hp[1]);

    /* 4a: a repeated nonce/frame is rejected once rx_ctr advanced. */
    if (secure_pair(hp, &a, &z)) return 1;
    raw_frame(&a, hp[0], 0, 7, pt, 5, 0, 0);
    raw_frame(&a, hp[0], 0, 7, pt, 5, 0, 0);
    if (lmb_secure_recv(&z, hp[1], &tm)) { printf("  FAIL first replay frame\n"); fail = 1; }
    else lmb_msg_free(&tm);
    if (lmb_secure_recv(&z, hp[1], &tm) == 0) { printf("  FAIL replay accepted\n"); fail = 1; lmb_msg_free(&tm); }
    else printf("  ok   replayed frame rejected\n");
    close(hp[0]); close(hp[1]);

    /* 4b: the all-zero shared result from a low-order point is forbidden. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, hp)) return 1;
    pthread_t lo; pthread_create(&lo, NULL, srv_low_order, &hp[1]);
    if (lmb_secure_handshake(hp[0], 1, sk_c, pk_c, &a) == 0) {
        printf("  FAIL low-order X25519 point accepted\n"); fail = 1;
    } else printf("  ok   low-order X25519 point rejected\n");
    pthread_join(lo, NULL); close(hp[0]); close(hp[1]);

    /* 5: first contact is persisted; a different key for the same endpoint
     * is rejected.  Strict operator pins reject missing endpoints as well. */
    setenv("LUMABRI_KNOWN_HOSTS", argv[1], 1);
    if (lmb_sec_check_peer("node:1", pk_s) ||
        lmb_sec_check_peer("node:1", pk_s) ||
        lmb_sec_check_peer("node:1", pk_c) == 0) {
        printf("  FAIL persistent TOFU key-change check\n"); fail = 1;
    } else printf("  ok   persistent TOFU rejects endpoint key change\n");
    setenv("LUMABRI_REQUIRE_PIN", "1", 1);
    if (lmb_sec_check_peer("missing:2", pk_s) == 0) {
        printf("  FAIL strict pin mode learned a missing endpoint\n"); fail = 1;
    } else printf("  ok   strict mode refuses an unpinned endpoint\n");
    unsetenv("LUMABRI_REQUIRE_PIN");

    char pins[512]; snprintf(pins, sizeof pins, "%s.pins", argv[1]);
    FILE *pf = fopen(pins, "w");
    char hs[65], hc[65]; lmb_hex(hs, pk_s, 32); lmb_hex(hc, pk_c, 32);
    int pbad = !pf;
    if (pf) {
        if (fprintf(pf, "rotate:4 %s\nrotate:4 %s\n", hs, hc) < 0) pbad = 1;
        if (fclose(pf)) pbad = 1;
    }
    if (pbad || lmb_sec_pin_file(pins, "rotate:4", pk_s, 0) ||
        lmb_sec_pin_file(pins, "rotate:4", pk_c, 0)) {
        printf("  FAIL strict pin overlap rotation\n"); fail = 1;
    } else printf("  ok   strict pins accept old+new rotation overlap\n");

    /* 6: exercise the transparent registry and its close lifecycle. */
    setenv("LUMABRI_ENCRYPT", "1", 1);
    if (lmb_secure_init()) { printf("  FAIL secure init\n"); fail = 1; }
    else {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, hp)) return 1;
        WrapArg wa = { hp[1], g_sec_pk };
        pthread_t wt; pthread_create(&wt, NULL, srv_wrap, &wa);
        int wr = lmb_sec_wrap_hook(hp[0], 1, "registry:3");
        pthread_join(wt, NULL);
        /* Both wrappers use this process's configured identity; the purpose
         * here is registry lifecycle, while the direct handshake above
         * independently proved distinct peer identities. */
        if (wr || !registered(hp[0]) || !registered(hp[1]) ||
            !lmb_secure_peer_matches(hp[1], g_sec_pk)) {
            printf("  FAIL encrypted fd registration\n"); fail = 1;
        } else {
            lmb_close(hp[0]);
            if (registered(hp[0])) { printf("  FAIL close retained session\n"); fail = 1; }
            else printf("  ok   close removes encrypted fd session\n");
            LmbSecEntry *held = lmb_sec_acquire(hp[1]);
            lmb_sec_forget_hook(hp[1]);
            if (!held || registered(hp[1])) {
                printf("  FAIL concurrent session retirement\n"); fail = 1;
            } else printf("  ok   active I/O reference defers session destruction\n");
            if (held) lmb_sec_release(held);
            lmb_close(hp[1]);
        }
    }

    puts(fail ? "LUMABRI SECURE TEST: FAIL" : "LUMABRI SECURE TEST: PASS");
    return fail;
}
EOF
cc -O2 -w -I. -pthread "$T/t.c" -o "$T/t"
"$T/t" "$T/known_hosts"
