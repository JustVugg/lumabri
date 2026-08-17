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
 * and replay. LUMABRI_PEER_PINS provides strict out-of-band endpoint pinning;
 * without it, persistent known_hosts provides the SSH first-connect model.
 * LUMABRI_PUBKEY is separate: it authenticates model contents, not endpoints.
 */
#ifndef LUMABRI_SECURE_H
#define LUMABRI_SECURE_H

#include "lumabri_proto.h"
#include "lumabri_crypto.h"
#include "lumabri_sign.h"
#include <ctype.h>
#include <sys/file.h>

#ifndef EKEYREJECTED
#define EKEYREJECTED EACCES
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
static void lmb_hs_deadline(int fd, int on) {
    int ms = on ? lmb_env_int("LUMABRI_HS_TIMEOUT_MS", 8000, 100, 60000)
                : lmb_env_int("LUMABRI_IO_TIMEOUT_MS",
                              LMB_DEFAULT_IO_TIMEOUT_MS, 100, 3600000);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static int lmb_secure_handshake(int fd, int is_client,
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
        if (lmb_read_full(fd, peer_eph, 32)) return -1;
        memcpy(eph_c, eph_pk, 32); memcpy(eph_s, peer_eph, 32);
    } else {
        if (lmb_read_full(fd, peer_eph, 32)) return -1;
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
        if (lmb_read_full(fd, peer_id, 32) || lmb_read_full(fd, peer_sig, 64)) return -1;
        if (lmb_sign_verify(peer_sig, tr, tl, peer_id)) { errno = EKEYREJECTED; return -1; }
        if (lmb_write_full(fd, id_pk, 32) || lmb_write_full(fd, my_sig, 64)) return -1;
    } else {
        if (lmb_write_full(fd, id_pk, 32) || lmb_write_full(fd, my_sig, 64)) return -1;
        if (lmb_read_full(fd, peer_id, 32) || lmb_read_full(fd, peer_sig, 64)) return -1;
        if (lmb_sign_verify(peer_sig, tr, tl, peer_id)) { errno = EKEYREJECTED; return -1; }
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
static int lmb_secure_send(LmbSecure *s, int fd, uint32_t op,
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
    if (lmb_read_full(fd, wire, total) || lmb_read_full(fd, tag, 16)) {
        free(wire); lmb_rx_release(m); return -1;
    }
    uint8_t nonce[12]; lmb_sec_nonce(nonce, s->rx_ctr++);
    if (lc_aead_open(s->rx_key, nonce, hdr, 16, wire, total, tag, wire)) {
        free(wire); lmb_rx_release(m); return -1;  /* tamper, replay, or wrong key */
    }
    m->storage = wire;
    if (m->body_len) m->body = wire;
    if (m->pay_len) m->pay = wire + m->body_len;
    return 0;
}

/* ---- peer pinning and persistent TOFU ----------------------------------
 *
 * A self-signed handshake proves possession of *a* key, not that it is the
 * key belonging to the endpoint the caller intended.  Outbound connections
 * therefore check "address -> Ed25519 key" here before any application frame
 * (and thus before an invite token or activation) is sent.
 *
 * LUMABRI_PEER_PINS points at a strict, operator-managed file.  Every outbound
 * address must occur in it.  Otherwise ~/.lumabri/known_hosts is persistent
 * TOFU: first contact is recorded durably, later changes are refused.
 * LUMABRI_REQUIRE_PIN=1 disables learning and accepts existing known_hosts
 * entries only.  File format: one `host:port 64hex` pair per line. */

static int lmb_sec_addr_ok(const char *addr) {
    if (!addr || !*addr || strlen(addr) >= 256) return 0;
    for (const unsigned char *p = (const unsigned char *)addr; *p; p++)
        if (isspace(*p) || *p < 0x21 || *p == 0x7f) return 0;
    return 1;
}

static const char *lmb_sec_known_hosts_path(char out[512]) {
    const char *e = getenv("LUMABRI_KNOWN_HOSTS");
    if (e && *e) { snprintf(out, 512, "%s", e); return out; }
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char dir[448];
    if (snprintf(dir, sizeof dir, "%s/.lumabri", home) >= (int)sizeof dir) return NULL;
    if (mkdir(dir, 0700) && errno != EEXIST) return NULL;
    if (snprintf(out, 512, "%s/known_hosts", dir) >= 512) return NULL;
    return out;
}

/* Returns 0 match/learned, -1 mismatch/missing/error. */
static int lmb_sec_pin_file(const char *path, const char *addr,
                            const uint8_t peer[32], int learn) {
    int flags = learn ? (O_RDWR | O_CREAT) : O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        (learn && (st.st_uid != geteuid() || (st.st_mode & 022)))) {
        close(fd); errno = EACCES; return -1;
    }
    if (flock(fd, learn ? LOCK_EX : LOCK_SH)) { close(fd); return -1; }
    FILE *fp = fdopen(fd, learn ? "r+" : "r");
    if (!fp) { close(fd); return -1; }
    char line[768], host[256], hex[65], extra;
    int found = 0, seen_addr = 0, bad = 0;
    rewind(fp);
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        int n = sscanf(p, "%255s %64s %c", host, hex, &extra);
        if (n != 2) { bad = 1; break; }
        if (strcmp(host, addr)) continue;
        uint8_t key[32];
        if (lmb_unhex(key, hex, sizeof key)) { bad = 1; break; }
        if (learn) {
            /* known_hosts owns exactly one key per endpoint. */
            if (seen_addr++ || memcmp(key, peer, 32)) { bad = 1; break; }
            found = 1;
        } else if (!memcmp(key, peer, 32)) {
            /* An operator pin set may carry old+new rows during rotation. */
            found = 1;
        }
    }
    if (!bad && !found && learn) {
        char ph[65]; lmb_hex(ph, peer, 32);
        if (fseek(fp, 0, SEEK_END) || fprintf(fp, "%s %s\n", addr, ph) < 0 ||
            fflush(fp) || fsync(fd)) bad = 1;
        else found = 1;
    }
    fclose(fp); /* also unlocks and closes fd */
    return !bad && found ? 0 : -1;
}

static int lmb_sec_check_peer(const char *addr, const uint8_t peer[32]) {
    if (!lmb_sec_addr_ok(addr)) return -1;
    const char *pins = getenv("LUMABRI_PEER_PINS");
    if (pins && *pins) return lmb_sec_pin_file(pins, addr, peer, 0);
    char path[512];
    if (!lmb_sec_known_hosts_path(path)) return -1;
    int require = lmb_env_int("LUMABRI_REQUIRE_PIN", 0, 0, 1);
    return lmb_sec_pin_file(path, addr, peer, !require);
}

/* ---- opt-in transport integration -------------------------------------
 * A component calls lmb_secure_init() once at startup. When LUMABRI_ENCRYPT
 * is set it loads this machine's peer key and installs the proto.h hooks, so
 * every lmb_connect handshakes outbound and every lmb_secure_server() wraps an
 * accepted fd, and lmb_send/lmb_recv transparently use the AEAD channel for
 * any handshaked fd. Off by default: nothing changes unless asked.
 */
#include <pthread.h>

#define LMB_SEC_MAXFD 65536
typedef struct {
    LmbSecure sec;
    unsigned refs;
    int closing;
    pthread_mutex_t tx_lk, rx_lk;
} LmbSecEntry;
static LmbSecEntry *g_sec_reg[LMB_SEC_MAXFD];
static pthread_mutex_t g_sec_lk = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_sec_sk[64], g_sec_pk[32];
static int g_sec_enabled, g_sec_failed;

static void lmb_sec_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

static void lmb_sec_entry_free(LmbSecEntry *e) {
    pthread_mutex_destroy(&e->tx_lk);
    pthread_mutex_destroy(&e->rx_lk);
    lmb_sec_wipe(&e->sec, sizeof e->sec);
    free(e);
}

/* Take a lifetime reference before dropping the registry lock.  close() may
 * run on another application thread while send/recv is blocked; without this
 * reference it could free the session under the AEAD operation. */
static LmbSecEntry *lmb_sec_acquire(int fd) {
    if (fd < 0 || fd >= LMB_SEC_MAXFD) return NULL;
    pthread_mutex_lock(&g_sec_lk);
    LmbSecEntry *e = g_sec_reg[fd];
    if (e && !e->closing) e->refs++;
    else e = NULL;
    pthread_mutex_unlock(&g_sec_lk);
    return e;
}

static void lmb_sec_release(LmbSecEntry *e) {
    int reap = 0;
    pthread_mutex_lock(&g_sec_lk);
    if (e->refs) e->refs--;
    if (e->closing && !e->refs) reap = 1;
    pthread_mutex_unlock(&g_sec_lk);
    if (reap) lmb_sec_entry_free(e);
}

static int lmb_sec_send_hook(int fd, uint32_t op, const void *b, uint32_t bl,
                             const void *p, uint32_t pl) {
    LmbSecEntry *e = lmb_sec_acquire(fd);
    if (!e) return -2;                     /* not an encrypted fd: plaintext */
    pthread_mutex_lock(&e->tx_lk);         /* counters and frames stay ordered */
    int rc = lmb_secure_send(&e->sec, fd, op, b, bl, p, pl);
    pthread_mutex_unlock(&e->tx_lk);
    lmb_sec_release(e);
    return rc;
}
static int lmb_sec_recv_hook(int fd, LmbMsg *m) {
    LmbSecEntry *e = lmb_sec_acquire(fd);
    if (!e) return -2;
    pthread_mutex_lock(&e->rx_lk);
    int rc = lmb_secure_recv(&e->sec, fd, m);
    pthread_mutex_unlock(&e->rx_lk);
    lmb_sec_release(e);
    return rc;
}
static void lmb_sec_forget_hook(int fd) {
    if (fd < 0 || fd >= LMB_SEC_MAXFD) return;
    int reap = 0;
    pthread_mutex_lock(&g_sec_lk);
    LmbSecEntry *e = g_sec_reg[fd];
    g_sec_reg[fd] = NULL;
    if (e) { e->closing = 1; if (!e->refs) reap = 1; }
    pthread_mutex_unlock(&g_sec_lk);
    if (reap) lmb_sec_entry_free(e);
}

static int lmb_sec_wrap_hook(int fd, int is_client, const char *addr) {
    if (fd < 0 || fd >= LMB_SEC_MAXFD) return -1;
    LmbSecEntry *e = (LmbSecEntry *)calloc(1, sizeof *e);
    if (!e) return -1;
    if (pthread_mutex_init(&e->tx_lk, NULL)) { free(e); return -1; }
    if (pthread_mutex_init(&e->rx_lk, NULL)) {
        pthread_mutex_destroy(&e->tx_lk); free(e); return -1;
    }
    if (lmb_secure_handshake(fd, is_client, g_sec_sk, g_sec_pk, &e->sec) ||
        (is_client && lmb_sec_check_peer(addr, e->sec.peer_id))) {
        lmb_sec_entry_free(e); return -1;
    }
    lmb_sec_forget_hook(fd);                 /* fd reuse drops the old keys */
    pthread_mutex_lock(&g_sec_lk);
    g_sec_reg[fd] = e;
    pthread_mutex_unlock(&g_sec_lk);
    return 0;
}

static int lmb_sec_reject_send(int fd, uint32_t op, const void *b, uint32_t bl,
                               const void *p, uint32_t pl) {
    (void)fd; (void)op; (void)b; (void)bl; (void)p; (void)pl;
    errno = EACCES; return -1;
}
static int lmb_sec_reject_recv(int fd, LmbMsg *m) {
    (void)fd; (void)m; errno = EACCES; return -1;
}
static int lmb_sec_reject_wrap(int fd, int is_client, const char *addr) {
    (void)fd; (void)is_client; (void)addr; errno = EACCES; return -1;
}

/* handshake an accepted (inbound) fd; 0 when encryption is off or the wrap
 * succeeds, -1 when an enabled handshake fails and the caller must drop it */
static LMB_MAYBE_UNUSED int lmb_secure_server(int fd) {
    if (!lmb_enc_wrap) return 0;
    return lmb_enc_wrap(fd, 0, NULL);
}

/* 1 when fd is encrypted and its handshake identity equals pk; 0 otherwise. */
static LMB_MAYBE_UNUSED int lmb_secure_peer_matches(int fd,
                                                    const uint8_t pk[32]) {
    LmbSecEntry *e = lmb_sec_acquire(fd);
    if (!e) return 0;
    int match = e->sec.active && e->sec.have_peer_id &&
                !memcmp(e->sec.peer_id, pk, 32);
    lmb_sec_release(e);
    return match;
}

static LMB_MAYBE_UNUSED int lmb_secure_enabled(void) {
    return g_sec_enabled && !g_sec_failed;
}

static int lmb_secure_init(void) {
    const char *e = getenv("LUMABRI_ENCRYPT");
    if (!e || !e[0] || e[0] == '0') return 0;
    if (g_sec_enabled) return g_sec_failed ? -1 : 0;
    g_sec_enabled = 1;
    char kp[512];
    if (lmb_peer_identity(lmb_peer_key_path(kp, sizeof kp), g_sec_sk, g_sec_pk)) {
        fprintf(stderr, "[lumabri] LUMABRI_ENCRYPT set but no peer key at %s — "
                        "refusing all network traffic\n", kp);
        g_sec_failed = 1;
        lmb_enc_send = lmb_sec_reject_send;
        lmb_enc_recv = lmb_sec_reject_recv;
        lmb_enc_wrap = lmb_sec_reject_wrap;
        return -1;
    }
    lmb_enc_send = lmb_sec_send_hook;
    lmb_enc_recv = lmb_sec_recv_hook;
    lmb_enc_wrap = lmb_sec_wrap_hook;
    lmb_enc_forget = lmb_sec_forget_hook;
    fprintf(stderr, "[lumabri] transport encryption on "
                    "(X25519 + ChaCha20-Poly1305, pinned/persistent-TOFU identity)\n");
    return 0;
}

/* Source files including this header get lifecycle-safe closes automatically.
 * The LD_PRELOAD shim defines its own close interposer and opts out, then calls
 * lmb_sec_forget_hook itself. */
#ifndef LMB_SECURE_NO_CLOSE_REDIRECT
#define close(fd) lmb_close(fd)
#endif

#endif /* LUMABRI_SECURE_H */
