/* Authenticated channel over native sockets. Identity storage and endpoint
 * trust remain the caller's responsibility; this is the shared handshake and
 * AEAD implementation, not a second protocol or an automatic trust policy. */
#ifndef LUMABRI_SECURE_CHANNEL_H
#define LUMABRI_SECURE_CHANNEL_H
#include "lumabri_proto.h"
#include "lumabri_crypto.h"
#include "src/crypto/lumabri_random.h"

/* Linux has a key-specific errno; Darwin uses EAUTH. Authentication still
 * fails closed on systems that only expose the POSIX permission error. */
#if defined(EKEYREJECTED)
#define LMB_AUTH_REJECTED EKEYREJECTED
#elif defined(EAUTH)
#define LMB_AUTH_REJECTED EAUTH
#else
#define LMB_AUTH_REJECTED EACCES
#endif

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
/* seconds a handshake may block, so a plaintext or hostile connection cannot
 * pin a worker for the bulk io timeout; LUMABRI_HS_TIMEOUT_MS overrides */
static void lmb_hs_deadline(LmbSocket fd, int on) {
    int ms = on ? lmb_env_int("LUMABRI_HS_TIMEOUT_MS", 8000, 100, 60000)
                : lmb_env_int("LUMABRI_IO_TIMEOUT_MS",
                              LMB_DEFAULT_IO_TIMEOUT_MS, 100, 3600000);
    (void)lmb_socket_timeout(fd, ms);
}

static int lmb_secure_handshake(LmbSocket fd, int is_client,
                                const uint8_t id_sk[64], const uint8_t id_pk[32],
                                LmbSecure *s) {
    memset(s, 0, sizeof *s);
    lmb_hs_deadline(fd, 1);
    uint8_t eph_sk[32], eph_pk[32], peer_eph[32];
    lmb_random(eph_sk, 32);
    lc_x25519_base(eph_pk, eph_sk);

    uint8_t eph_c[32], eph_s[32];
    if (is_client) {
        if (lmb_write_full(fd, eph_pk, 32)) return -1;
        if (lmb_socket_read_full(fd, peer_eph, 32)) return -1;
        memcpy(eph_c, eph_pk, 32); memcpy(eph_s, peer_eph, 32);
    } else {
        if (lmb_socket_read_full(fd, peer_eph, 32)) return -1;
        if (lmb_write_full(fd, eph_pk, 32)) return -1;
        memcpy(eph_c, peer_eph, 32); memcpy(eph_s, eph_pk, 32);
    }

    /* RFC 7748 permits protocols to reject the all-zero result.  We must:
     * accepting a low-order public point destroys contributory behaviour and
     * produces a publicly predictable traffic secret. */
    uint8_t shared[32], shared_or = 0;
    lc_x25519(shared, eph_sk, peer_eph);
    for (size_t i = 0; i < sizeof shared; i++) shared_or |= shared[i];
    if (!shared_or) { errno = EPROTO; return -1; }

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
        if (lmb_socket_read_full(fd, peer_id, 32) || lmb_socket_read_full(fd, peer_sig, 64)) return -1;
        if (lmb_sign_verify(peer_sig, tr, tl, peer_id)) { errno = LMB_AUTH_REJECTED; return -1; }
        if (lmb_write_full(fd, id_pk, 32) || lmb_write_full(fd, my_sig, 64)) return -1;
    } else {
        if (lmb_write_full(fd, id_pk, 32) || lmb_write_full(fd, my_sig, 64)) return -1;
        if (lmb_socket_read_full(fd, peer_id, 32) || lmb_socket_read_full(fd, peer_sig, 64)) return -1;
        if (lmb_sign_verify(peer_sig, tr, tl, peer_id)) { errno = LMB_AUTH_REJECTED; return -1; }
    }
    memcpy(s->peer_id, peer_id, 32); s->have_peer_id = 1;

    uint8_t info[64], keys[64];
    memcpy(info, eph_c, 32); memcpy(info + 32, eph_s, 32);
    if (lc_hkdf((const uint8_t *)"lumabri-secure-v1", 17,
                shared, 32, info, 64, keys, 64)) {
        errno = EOVERFLOW;
        return -1;
    }
    /* client sends with keys[0..31], server sends with keys[32..63] */
    if (is_client) { memcpy(s->tx_key, keys, 32); memcpy(s->rx_key, keys + 32, 32); }
    else           { memcpy(s->tx_key, keys + 32, 32); memcpy(s->rx_key, keys, 32); }
    s->tx_ctr = s->rx_ctr = 0;
    s->active = 1;
    lmb_hs_deadline(fd, 0);            /* back to the normal io timeout */
    memset(eph_sk, 0, sizeof eph_sk);
    memset(shared, 0, sizeof shared);
    memset(keys, 0, sizeof keys);
    return 0;
}

/* Encrypted frame on the wire: u32 magic, u32 op, u32 body_len, u32 pay_len,
 * then AEAD(ciphertext = body||pay) with a 16-byte tag. The 16-byte header is
 * the AEAD's associated data, so lengths and op are authenticated too. */
static int lmb_secure_send(LmbSecure *s, LmbSocket fd, uint32_t op,
                           const void *body, uint32_t body_len,
                           const void *pay, uint32_t pay_len) {
    if (!s->active) return lmb_send(fd, op, body, body_len, pay, pay_len);
    if (!lmb_frame_shape_ok(op, body_len, pay_len)) { errno = EMSGSIZE; return -1; }
    uint32_t total = body_len + pay_len;
    uint8_t hdr[16];
    lc_st32(hdr, LMB_MAGIC); lc_st32(hdr + 4, op);
    lc_st32(hdr + 8, body_len); lc_st32(hdr + 12, pay_len);
    uint8_t *ct = (uint8_t *)malloc(total ? total : 1);
    uint8_t tag[16];
    if (!ct) return -1;
    if (body_len) memcpy(ct, body, body_len);
    if (pay_len)  memcpy(ct + body_len, pay, pay_len);
    uint8_t nonce[12]; lmb_sec_nonce(nonce, s->tx_ctr++);
    /* ChaCha20 is safe in place, so an outbound 64 MiB frame also needs only
     * one frame-sized allocation. */
    lc_aead_seal(s->tx_key, nonce, hdr, 16, ct, total, ct, tag);
    int rc = lmb_write_full(fd, hdr, 16) || lmb_write_full(fd, ct, total) ||
             lmb_write_full(fd, tag, 16);
    free(ct);
    return rc ? -1 : 0;
}

static int lmb_secure_recv(LmbSecure *s, LmbSocket fd, LmbMsg *m) {
    if (!s->active) return lmb_recv(fd, m);
    memset(m, 0, sizeof *m);
    uint8_t hdr[16];
    if (lmb_socket_read_full(fd, hdr, 16)) return -1;
    if (lc_ld32(hdr) != LMB_MAGIC) { errno = EPROTO; return -1; }
    m->op = lc_ld32(hdr + 4);
    m->body_len = lc_ld32(hdr + 8);
    m->pay_len = lc_ld32(hdr + 12);
    if (!lmb_frame_shape_ok(m->op, m->body_len, m->pay_len)) { errno = EMSGSIZE; return -1; }
    uint64_t total64 = (uint64_t)m->body_len + m->pay_len;
    if (total64 > UINT32_MAX) { errno = EMSGSIZE; return -1; }
    uint32_t total = (uint32_t)total64;
    if (lmb_rx_reserve(m, total)) return -1;
    /* AEAD open is safe in-place: authenticate ciphertext first, then XOR the
     * ChaCha stream over the same bytes.  One legal 64 MiB frame therefore
     * costs 64 MiB, not two or three copies. */
    uint8_t *wire = (uint8_t *)malloc(total ? total : 1);
    uint8_t tag[16];
    if (!wire) { lmb_rx_release(m); return -1; }
    if (lmb_socket_read_full(fd, wire, total) || lmb_socket_read_full(fd, tag, 16)) {
        free(wire); lmb_rx_release(m); return -1;
    }
    uint8_t nonce[12]; lmb_sec_nonce(nonce, s->rx_ctr++);
    if (lc_aead_open(s->rx_key, nonce, hdr, 16, wire, total, tag, wire)) {
        free(wire); lmb_rx_release(m); errno = EBADMSG; return -1; /* tamper, replay, or wrong key */
    }
    m->storage = wire;
    if (m->body_len) m->body = wire;
    if (m->pay_len) m->pay = wire + m->body_len;
    return 0;
}

#endif /* LUMABRI_SECURE_CHANNEL_H */
