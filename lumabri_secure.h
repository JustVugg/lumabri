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
#ifndef _WIN32
#include <sys/file.h>
#endif

#include "src/protocol/lumabri_secure_channel.h"

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
#ifdef _WIN32
    return lmb_private_default_path(out, 512, "known_hosts") ? NULL : out;
#else
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    char dir[448];
    if (snprintf(dir, sizeof dir, "%s/.lumabri", home) >= (int)sizeof dir) return NULL;
    if (mkdir(dir, 0700) && errno != EEXIST) return NULL;
    if (snprintf(out, 512, "%s/known_hosts", dir) >= 512) return NULL;
    return out;
#endif
}

/* Returns 0 match/learned, -1 mismatch/missing/error. */
static int lmb_sec_pin_file(const char *path, const char *addr,
                            const uint8_t peer[32], int learn) {
    if (!lmb_sec_addr_ok(addr)) { errno = EINVAL; return -1; }
#ifdef _WIN32
    FILE *fp = lmb_private_stream(path, learn, learn);
    if (!fp) return -1;
#else
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
#endif
    char line[768], host[256], hex[65], extra;
    int found = 0, seen_addr = 0, bad = 0;
    rewind(fp);
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        int n = sscanf(p, "%255s %64s %c", host, hex, &extra);
        if (n != 2 || strlen(hex) != 64 || !lmb_sec_addr_ok(host)) { bad = 1; break; }
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
    if (ferror(fp)) bad = 1;
    if (!bad && !found && learn) {
        char ph[65]; lmb_hex(ph, peer, 32);
        if (fseek(fp, 0, SEEK_END) || fprintf(fp, "%s %s\n", addr, ph) < 0 ||
#ifdef _WIN32
            lmb_private_stream_flush(fp)) bad = 1;
#else
            fflush(fp) || fsync(fd)) bad = 1;
#endif
        else found = 1;
    }
#ifdef _WIN32
    lmb_private_stream_close(fp);
#else
    fclose(fp); /* also unlocks and closes fd */
#endif
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
#define LMB_SEC_BUCKETS 1024
typedef struct LmbSecEntry {
    LmbSocket fd;
    struct LmbSecEntry *next;
    LmbSecure sec;
    unsigned refs;
    int closing;
    pthread_mutex_t tx_lk, rx_lk;
} LmbSecEntry;
static LmbSecEntry *g_sec_reg[LMB_SEC_BUCKETS];
static size_t g_sec_count;
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
static int lmb_sec_fd_valid(LmbSocket fd) {
#ifdef _WIN32
    return fd != LMB_INVALID_SOCKET;
#else
    return fd >= 0;
#endif
}
/* SOCKET values are not small CRT indices. A bounded table keys the full
 * handle on both platforms; collisions never share identity or nonce state. */
static size_t lmb_sec_bucket(LmbSocket fd) {
    uint64_t key = (uint64_t)(uintptr_t)fd;
    key ^= key >> 32; key ^= key >> 16;
    return (size_t)key & (LMB_SEC_BUCKETS - 1);
}
static LmbSecEntry *lmb_sec_acquire(LmbSocket fd) {
    if (!lmb_sec_fd_valid(fd)) return NULL;
    pthread_mutex_lock(&g_sec_lk);
    LmbSecEntry *e = g_sec_reg[lmb_sec_bucket(fd)];
    while (e && e->fd != fd) e = e->next;
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

static int lmb_sec_send_hook(LmbSocket fd, uint32_t op, const void *b, uint32_t bl,
                             const void *p, uint32_t pl) {
    LmbSecEntry *e = lmb_sec_acquire(fd);
    if (!e) return -2;                     /* not an encrypted fd: plaintext */
    pthread_mutex_lock(&e->tx_lk);         /* counters and frames stay ordered */
    int rc = lmb_secure_send(&e->sec, fd, op, b, bl, p, pl);
    pthread_mutex_unlock(&e->tx_lk);
    lmb_sec_release(e);
    return rc;
}
static int lmb_sec_recv_hook(LmbSocket fd, LmbMsg *m) {
    LmbSecEntry *e = lmb_sec_acquire(fd);
    if (!e) return -2;
    pthread_mutex_lock(&e->rx_lk);
    int rc = lmb_secure_recv(&e->sec, fd, m);
    pthread_mutex_unlock(&e->rx_lk);
    lmb_sec_release(e);
    return rc;
}
static void lmb_sec_forget_hook(LmbSocket fd) {
    if (!lmb_sec_fd_valid(fd)) return;
    int reap = 0;
    pthread_mutex_lock(&g_sec_lk);
    LmbSecEntry **slot = &g_sec_reg[lmb_sec_bucket(fd)];
    while (*slot && (*slot)->fd != fd) slot = &(*slot)->next;
    LmbSecEntry *e = *slot;
    if (e) { *slot = e->next; g_sec_count--; }
    if (e) { e->closing = 1; if (!e->refs) reap = 1; }
    pthread_mutex_unlock(&g_sec_lk);
    if (reap) lmb_sec_entry_free(e);
}

static int lmb_sec_wrap_hook(LmbSocket fd, int is_client, const char *addr) {
    if (!lmb_sec_fd_valid(fd)) return -1;
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
    if (g_sec_count >= LMB_SEC_MAXFD) {
        pthread_mutex_unlock(&g_sec_lk); lmb_sec_entry_free(e); errno = ENOBUFS; return -1;
    }
    e->fd = fd;
    size_t bucket = lmb_sec_bucket(fd);
    e->next = g_sec_reg[bucket]; g_sec_reg[bucket] = e; g_sec_count++;
    pthread_mutex_unlock(&g_sec_lk);
    return 0;
}

static int lmb_sec_reject_send(LmbSocket fd, uint32_t op, const void *b, uint32_t bl,
                               const void *p, uint32_t pl) {
    (void)fd; (void)op; (void)b; (void)bl; (void)p; (void)pl;
    errno = EACCES; return -1;
}
static int lmb_sec_reject_recv(LmbSocket fd, LmbMsg *m) {
    (void)fd; (void)m; errno = EACCES; return -1;
}
static int lmb_sec_reject_wrap(LmbSocket fd, int is_client, const char *addr) {
    (void)fd; (void)is_client; (void)addr; errno = EACCES; return -1;
}

/* handshake an accepted (inbound) fd; 0 when encryption is off or the wrap
 * succeeds, -1 when an enabled handshake fails and the caller must drop it */
static LMB_MAYBE_UNUSED int lmb_secure_server(LmbSocket fd) {
    /* Replies use separate header/payload/tag writes just like requests.
     * Outbound TCP already disables Nagle; accepted sockets must too, before
     * the handshake and also in plaintext mode. Do not depend on listener
     * option inheritance (launchers may pass an already-bound descriptor).
     * Unix socketpairs used by local transports/tests have no TCP option. */
    struct sockaddr_storage address;
    LmbSocketLength address_size = sizeof address;
    if (getsockname(fd, (struct sockaddr *)&address, &address_size)) return -1;
    if (address.ss_family == AF_INET || address.ss_family == AF_INET6) {
        if (lmb_socket_nodelay(fd)) return -1;
    }
    if (!lmb_enc_wrap) return 0;
    return lmb_enc_wrap(fd, 0, NULL);
}

/* 1 when fd is encrypted and its handshake identity equals pk; 0 otherwise. */
static LMB_MAYBE_UNUSED int lmb_secure_peer_matches(LmbSocket fd,
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
#if !defined(LMB_SECURE_NO_CLOSE_REDIRECT) && !defined(_WIN32)
#define close(fd) lmb_close(fd)
#endif

#endif /* LUMABRI_SECURE_H */
