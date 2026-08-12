/* lumabri_secure.h — an authenticated encrypted channel over a TCP socket.
 *
 * Handshake (client initiates), authenticated by the peers' Ed25519 identity
 * keys from v0.7 so encryption is bound to identity, not anonymous:
 *
 *   c -> s : eph_c[32]                                     ephemeral X25519
 *   s -> c : eph_s[32] , id_s[32] , sig_s[64]             sig over eph_c|eph_s
 *   c -> s : id_c[32] , sig_c[64]                         sig over eph_c|eph_s
 *
 * shared = X25519(own ephemeral, peer ephemeral); session keys =
 * HKDF-SHA512(shared, "lumabri-secure-v1", eph_c|eph_s), split into one key
 * per direction. Every frame after that is ChaCha20-Poly1305 with a per-
 * direction counter nonce, so a passive observer reads nothing, a flipped
 * byte fails the tag, and a replayed frame fails the counter. The identity
 * signatures over the ephemeral exchange are what an active man in the middle
 * cannot forge without a key the peer would recognise as wrong.
 *
 * This defeats passive capture of tokens and activations and frame tampering
 * and replay. Full MITM resistance still needs the verifier to pin the peer's
 * identity out of band (LUMABRI_PUBKEY for a signed swarm); without a pin it
 * is trust-on-first-use, the SSH first-connect model.
 */
#ifndef LUMABRI_SECURE_H
#define LUMABRI_SECURE_H

#include "lumabri_proto.h"
#include "lumabri_crypto.h"
#include "lumabri_sign.h"

typedef struct {
    int active;
    uint8_t tx_key[32], rx_key[32];
    uint64_t tx_ctr, rx_ctr;
    uint8_t peer_id[32];        /* the Ed25519 identity that signed the handshake */
    int have_peer_id;
} LmbSecure;

#define LMB_HS_TAG "lumabri-hs-v1"

static void lmb_sec_nonce(uint8_t out[12], uint64_t ctr) {
    memset(out, 0, 4);
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(ctr >> (8 * i));
}

/* One handshake. `id_sk`/`id_pk` are this peer's Ed25519 identity (v0.7).
 * Returns 0 and fills `s`, or -1. */
static int lmb_secure_handshake(int fd, int is_client,
                                const uint8_t id_sk[64], const uint8_t id_pk[32],
                                LmbSecure *s) {
    memset(s, 0, sizeof *s);
    uint8_t eph_sk[32], eph_pk[32], peer_eph[32];
    lmb_random(eph_sk, 32);
    lc_x25519_base(eph_pk, eph_sk);

    uint8_t eph_c[32], eph_s[32];
    if (is_client) {
        if (lmb_write_full(fd, eph_pk, 32)) return -1;
        if (lmb_read_full(fd, peer_eph, 32)) return -1;
        memcpy(eph_c, eph_pk, 32); memcpy(eph_s, peer_eph, 32);
    } else {
        if (lmb_read_full(fd, peer_eph, 32)) return -1;
        if (lmb_write_full(fd, eph_pk, 32)) return -1;
        memcpy(eph_c, peer_eph, 32); memcpy(eph_s, eph_pk, 32);
    }

    /* transcript both sides sign: tag || client ephemeral || server ephemeral */
    uint8_t tr[sizeof(LMB_HS_TAG) - 1 + 64];
    size_t tl = 0;
    memcpy(tr + tl, LMB_HS_TAG, sizeof(LMB_HS_TAG) - 1); tl += sizeof(LMB_HS_TAG) - 1;
    memcpy(tr + tl, eph_c, 32); tl += 32;
    memcpy(tr + tl, eph_s, 32); tl += 32;

    uint8_t my_sig[64];
    lmb_sign(my_sig, tr, tl, id_sk);

    uint8_t peer_id[32], peer_sig[64];
    if (is_client) {
        /* the server sends its identity and signature right after its
         * ephemeral; read and verify them, then send ours */
        if (lmb_read_full(fd, peer_id, 32) || lmb_read_full(fd, peer_sig, 64)) return -1;
        if (lmb_sign_verify(peer_sig, tr, tl, peer_id)) return -1;
        if (lmb_write_full(fd, id_pk, 32) || lmb_write_full(fd, my_sig, 64)) return -1;
    } else {
        if (lmb_write_full(fd, id_pk, 32) || lmb_write_full(fd, my_sig, 64)) return -1;
        if (lmb_read_full(fd, peer_id, 32) || lmb_read_full(fd, peer_sig, 64)) return -1;
        if (lmb_sign_verify(peer_sig, tr, tl, peer_id)) return -1;
    }
    memcpy(s->peer_id, peer_id, 32); s->have_peer_id = 1;

    uint8_t shared[32];
    lc_x25519(shared, eph_sk, peer_eph);

    uint8_t info[64], keys[64];
    memcpy(info, eph_c, 32); memcpy(info + 32, eph_s, 32);
    lc_hkdf((const uint8_t *)"lumabri-secure-v1", 17, shared, 32, info, 64, keys, 64);
    /* client sends with keys[0..31], server sends with keys[32..63] */
    if (is_client) { memcpy(s->tx_key, keys, 32); memcpy(s->rx_key, keys + 32, 32); }
    else           { memcpy(s->tx_key, keys + 32, 32); memcpy(s->rx_key, keys, 32); }
    s->tx_ctr = s->rx_ctr = 0;
    s->active = 1;
    return 0;
}

/* Encrypted frame on the wire: u32 magic, u32 op, u32 body_len, u32 pay_len,
 * then AEAD(ciphertext = body||pay) with a 16-byte tag. The 16-byte header is
 * the AEAD's associated data, so lengths and op are authenticated too. */
static int lmb_secure_send(LmbSecure *s, int fd, uint32_t op,
                           const void *body, uint32_t body_len,
                           const void *pay, uint32_t pay_len) {
    if (!s->active) return lmb_send(fd, op, body, body_len, pay, pay_len);
    if (!lmb_frame_shape_ok(op, body_len, pay_len)) { errno = EMSGSIZE; return -1; }
    uint32_t total = body_len + pay_len;
    uint8_t hdr[16];
    lc_st32(hdr, LMB_MAGIC); lc_st32(hdr + 4, op);
    lc_st32(hdr + 8, body_len); lc_st32(hdr + 12, pay_len);
    uint8_t *pt = (uint8_t *)malloc(total ? total : 1);
    uint8_t *ct = (uint8_t *)malloc(total ? total : 1);
    uint8_t tag[16];
    if (!pt || !ct) { free(pt); free(ct); return -1; }
    if (body_len) memcpy(pt, body, body_len);
    if (pay_len)  memcpy(pt + body_len, pay, pay_len);
    uint8_t nonce[12]; lmb_sec_nonce(nonce, s->tx_ctr++);
    lc_aead_seal(s->tx_key, nonce, hdr, 16, pt, total, ct, tag);
    int rc = lmb_write_full(fd, hdr, 16) || lmb_write_full(fd, ct, total) ||
             lmb_write_full(fd, tag, 16);
    free(pt); free(ct);
    return rc ? -1 : 0;
}

static int lmb_secure_recv(LmbSecure *s, int fd, LmbMsg *m) {
    if (!s->active) return lmb_recv(fd, m);
    memset(m, 0, sizeof *m);
    uint8_t hdr[16];
    if (lmb_read_full(fd, hdr, 16)) return -1;
    if (lc_ld32(hdr) != LMB_MAGIC) return -1;
    m->op = lc_ld32(hdr + 4);
    m->body_len = lc_ld32(hdr + 8);
    m->pay_len = lc_ld32(hdr + 12);
    if (!lmb_frame_shape_ok(m->op, m->body_len, m->pay_len)) return -1;
    uint32_t total = m->body_len + m->pay_len;
    uint8_t *ct = (uint8_t *)malloc(total ? total : 1);
    uint8_t *pt = (uint8_t *)malloc(total ? total : 1);
    uint8_t tag[16];
    if (!ct || !pt) { free(ct); free(pt); return -1; }
    if (lmb_read_full(fd, ct, total) || lmb_read_full(fd, tag, 16)) { free(ct); free(pt); return -1; }
    uint8_t nonce[12]; lmb_sec_nonce(nonce, s->rx_ctr++);
    if (lc_aead_open(s->rx_key, nonce, hdr, 16, ct, total, tag, pt)) {
        free(ct); free(pt); return -1;             /* tamper, replay, or wrong key */
    }
    free(ct);
    if (m->body_len) { m->body = (uint8_t *)malloc(m->body_len); memcpy(m->body, pt, m->body_len); }
    if (m->pay_len)  { m->pay  = (uint8_t *)malloc(m->pay_len);  memcpy(m->pay, pt + m->body_len, m->pay_len); }
    free(pt);
    return 0;
}

#endif /* LUMABRI_SECURE_H */
