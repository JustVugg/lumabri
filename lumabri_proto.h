/* lumabri_proto.h — the lumabri wire protocol. Header-only, no dependencies.
 *
 * One frame on the wire:
 *   preamble  16 bytes  { u32 magic "LMB1", u32 op, u32 body_len, u32 pay_len }
 *   body      op-specific fields, little-endian, length-prefixed strings
 *   pay       raw file bytes (READ_R only)
 *
 * The body/pay split keeps metadata parsing away from bulk bytes: a reader
 * never scans file data looking for a terminator, and a 64 MiB cap on pay
 * bounds every allocation. Unknown ops get LMB_ERR back — same forward-compat
 * stance as the engine's serve protocol: reject loudly, never guess.
 *
 * Ops:
 *   PING → OK                                  liveness
 *   MANIFEST → MANIFEST_R                      files a maintainer holds
 *   READ{path,off,len} → READ_R(pay) | ERR     one byte range
 *   REGISTER{name,addr,files} → OK             maintainer → tracker heartbeat
 *   PLACEMENT → PLACEMENT_R                    file → peers map, tracker → chatter
 */
#ifndef LUMABRI_PROTO_H
#define LUMABRI_PROTO_H

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "src/protocol/lumabri_wire.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0     /* platforms without it deliver SIGPIPE; the
                              daemons ignore the signal themselves */
#endif

/* ---- full-buffer socket I/O ------------------------------------------- */

/* MSG_NOSIGNAL, not write(): this library also runs inside somebody else's
 * inference engine — via the LD_PRELOAD shim and the patched-engine client —
 * where it has no business changing the process's SIGPIPE disposition. A
 * peer that vanishes mid-frame must surface as EPIPE on this call, never as
 * a signal that kills the engine before failover can run. */
static int lmb_write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static _Thread_local uint64_t lmb_read_deadline_ms;

static uint64_t lmb_io_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int lmb_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        if (lmb_read_deadline_ms) {
            uint64_t now = lmb_io_monotonic_ms();
            if (!now || now >= lmb_read_deadline_ms) { errno = ETIMEDOUT; return -1; }
            uint64_t left = lmb_read_deadline_ms-now;
            struct pollfd ready = {fd, POLLIN, 0};
            int rc = poll(&ready, 1, left < 1000 ? (int)left : 1000);
            if (rc < 0 && errno != EINTR) return -1;
            if (rc <= 0) continue;
            if (!(ready.revents & POLLIN)) { errno = ECONNRESET; return -1; }
        }
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { errno = ECONNRESET; return -1; } /* EOF, not stale errno */
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* ---- frames ------------------------------------------------------------ */

/* Optional encrypted transport. lumabri_secure.h, when a component enables
 * it, points these at its AEAD channel: a fd that has been handshaked routes
 * through them, an ordinary fd falls through to plaintext (the hook returns
 * -2). Static per translation unit, so only a .c that opts in is affected. */
static int (*lmb_enc_send)(int, uint32_t, const void *, uint32_t, const void *, uint32_t);
static int (*lmb_enc_recv)(int, LmbMsg *);
static int (*lmb_enc_wrap)(int fd, int is_client, const char *addr);
static void (*lmb_enc_forget)(int fd);
static int lmb_env_int(const char *name, int fallback, int lo, int hi);

/* Bound aggregate receive memory, not just each individual frame.  Without
 * this, the connection gate multiplied by a legal 64 MiB payload is still an
 * easy multi-gigabyte OOM. */
static _Atomic uint64_t lmb_rx_inflight;
/* Optional tighter application bound, scoped to the receiving thread. Both
 * plaintext and authenticated readers apply it before allocating payloads. */
static _Thread_local uint32_t lmb_rx_frame_limit = UINT32_MAX;

static int lmb_rx_reserve(LmbMsg *m, uint32_t n) {
    if (n > lmb_rx_frame_limit) { errno = EMSGSIZE; return -1; }
    if (!n) return 0;
    uint64_t cap = (uint64_t)lmb_env_int("LUMABRI_RX_BUDGET_MIB", 256, 16, 4096)
                   << 20;
    uint64_t old = atomic_load(&lmb_rx_inflight);
    do {
        if ((uint64_t)n > cap || old > cap - n) { errno = ENOBUFS; return -1; }
    } while (!atomic_compare_exchange_weak(&lmb_rx_inflight, &old, old + n));
    m->rx_reserved = n;
    return 0;
}

static void lmb_rx_release(LmbMsg *m) {
    if (m->rx_reserved) {
        atomic_fetch_sub(&lmb_rx_inflight, m->rx_reserved);
        m->rx_reserved = 0;
    }
}

static int lmb_close(int fd) {
    if (lmb_enc_forget) lmb_enc_forget(fd);
    return close(fd);
}

static int lmb_send(int fd, uint32_t op, const void *body, uint32_t body_len,
                    const void *pay, uint32_t pay_len) {
    if (lmb_enc_send) { int r = lmb_enc_send(fd, op, body, body_len, pay, pay_len);
                        if (r != -2) return r; }
    if (!lmb_frame_shape_ok(op, body_len, pay_len)) { errno = EMSGSIZE; return -1; }
    uint8_t pre[16];
    lmb_put32(pre, LMB_MAGIC); lmb_put32(pre + 4, op);
    lmb_put32(pre + 8, body_len); lmb_put32(pre + 12, pay_len);
    if (lmb_write_full(fd, pre, 16)) return -1;
    if (body_len && lmb_write_full(fd, body, body_len)) return -1;
    if (pay_len && lmb_write_full(fd, pay, pay_len)) return -1;
    return 0;
}

/* Receives one frame; mallocs body/pay. Returns 0, or -1 on error/EOF. */
static int lmb_recv(int fd, LmbMsg *m) {
    if (lmb_enc_recv) { int r = lmb_enc_recv(fd, m); if (r != -2) return r; }
    uint8_t pre[16];
    memset(m, 0, sizeof *m);
    if (lmb_read_full(fd, pre, 16)) return -1;
    if (lmb_get32(pre) != LMB_MAGIC) { errno = EPROTO; return -1; }
    m->op = lmb_get32(pre + 4);
    m->body_len = lmb_get32(pre + 8);
    m->pay_len = lmb_get32(pre + 12);
    if (!lmb_frame_shape_ok(m->op, m->body_len, m->pay_len)) {
        errno = EMSGSIZE;
        return -1;
    }
    uint64_t total64 = (uint64_t)m->body_len + m->pay_len;
    if (total64 > UINT32_MAX || lmb_rx_reserve(m, (uint32_t)total64)) return -1;
    if (m->body_len) {
        if (!(m->body = (uint8_t *)malloc(m->body_len))) { lmb_rx_release(m); return -1; }
        if (lmb_read_full(fd, m->body, m->body_len)) {
            free(m->body); m->body = NULL; lmb_rx_release(m); return -1;
        }
    }
    if (m->pay_len) {
        if (!(m->pay = (uint8_t *)malloc(m->pay_len))) {
            free(m->body); m->body = NULL; lmb_rx_release(m); return -1;
        }
        if (lmb_read_full(fd, m->pay, m->pay_len)) {
            free(m->body); free(m->pay); m->body = m->pay = NULL;
            lmb_rx_release(m); return -1;
        }
    }
    return 0;
}

static inline int lmb_recv_limited(int fd, LmbMsg *m, uint32_t limit) {
    uint32_t previous = lmb_rx_frame_limit;
    lmb_rx_frame_limit = limit < previous ? limit : previous;
    int rc = lmb_recv(fd, m);
    lmb_rx_frame_limit = previous;
    return rc;
}

/* Absolute whole-frame deadline, not a timeout restarted by every byte. */
static inline int lmb_recv_bounded(int fd, LmbMsg *m, uint32_t limit, uint32_t timeout_ms) {
    uint64_t now = lmb_io_monotonic_ms(), previous = lmb_read_deadline_ms;
    if (!now || !timeout_ms) { memset(m, 0, sizeof *m); errno = EINVAL; return -1; }
    lmb_read_deadline_ms = previous && previous < now + timeout_ms ? previous : now + timeout_ms;
    int rc = lmb_recv_limited(fd, m, limit);
    lmb_read_deadline_ms = previous;
    return rc;
}

static void lmb_msg_free(LmbMsg *m) {
    if (m->storage) free(m->storage);
    else { free(m->body); free(m->pay); }
    m->body = m->pay = NULL;
    m->storage = NULL;
    lmb_rx_release(m);
}

/* Transfer a payload out of a message without relying on body/pay being
 * separate mallocs.  For a secure combined frame, compact the payload to the
 * front and transfer the one allocation. */
static LMB_MAYBE_UNUSED uint8_t *lmb_msg_take_pay(LmbMsg *m) {
    if (!m || !m->pay) return NULL;
    uint8_t *out;
    if (m->storage) {
        out = m->storage;
        if (m->pay != out) memmove(out, m->pay, m->pay_len);
        m->storage = NULL;
    } else {
        out = m->pay;
        free(m->body);
    }
    m->body = m->pay = NULL;
    lmb_rx_release(m);
    return out;
}

/* ---- body builder / cursor -------------------------------------------- */

typedef struct { uint8_t *p; size_t len, cap; } LmbBuf;

static int lmb_buf_reserve(LmbBuf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) return -1;
    size_t need = b->len + extra;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) return -1;
    b->p = p; b->cap = cap;
    return 0;
}
static int lmb_buf_u16(LmbBuf *b, uint16_t v) {
    if (lmb_buf_reserve(b, 2)) return -1;
    b->p[b->len++] = (uint8_t)v; b->p[b->len++] = (uint8_t)(v >> 8);
    return 0;
}
static int lmb_buf_u32(LmbBuf *b, uint32_t v) {
    if (lmb_buf_reserve(b, 4)) return -1;
    lmb_put32(b->p + b->len, v); b->len += 4;
    return 0;
}
static LMB_MAYBE_UNUSED int lmb_buf_u64(LmbBuf *b, uint64_t v) {
    if (lmb_buf_u32(b, (uint32_t)v)) return -1;
    return lmb_buf_u32(b, (uint32_t)(v >> 32));
}
static int lmb_buf_bytes(LmbBuf *b, const void *p, size_t n) {
    if (lmb_buf_reserve(b, n)) return -1;
    memcpy(b->p + b->len, p, n); b->len += n;
    return 0;
}
static int lmb_buf_str(LmbBuf *b, const char *s) {  /* u16 len + bytes */
    size_t n = strlen(s);
    if (n > 0xFFFF) return -1;
    if (lmb_buf_u16(b, (uint16_t)n)) return -1;
    if (lmb_buf_reserve(b, n)) return -1;
    memcpy(b->p + b->len, s, n); b->len += n;
    return 0;
}

/* Append the 100-byte peer-identity block a REGISTER/EREG ends with. The
 * caller signs {nonce,name,model,addr} with lmb_peer_auth_msg + lmb_sign;
 * this only lays the bytes down, so lumabri_proto.h stays crypto-free. */
static LMB_MAYBE_UNUSED int lmb_buf_peer_auth(LmbBuf *b, const uint8_t pk[32],
                                              const uint8_t sig[64]) {
    if (lmb_buf_u32(b, LMB_PEER_AUTH_MAGIC)) return -1;
    if (lmb_buf_bytes(b, pk, 32)) return -1;
    return lmb_buf_bytes(b, sig, 64);
}

/* Ask the tracker for this connection's identity nonce. 0 and fills nonce, or
 * -1. Sent once per control connection, right after connect (and AUTH). */
static LMB_MAYBE_UNUSED int lmb_request_challenge(int fd, uint8_t nonce[32]) {
    if (lmb_send(fd, LMB_CHALLENGE, NULL, 0, NULL, 0)) return -1;
    LmbMsg m;
    if (lmb_recv(fd, &m)) return -1;
    int ok = m.op == LMB_CHALLENGE_R && m.body_len == 32;
    if (ok) memcpy(nonce, m.body, 32);
    lmb_msg_free(&m);
    return ok ? 0 : -1;
}

typedef struct { const uint8_t *p; size_t len, off; } LmbCur;

static int lmb_cur_u16(LmbCur *c, uint16_t *v) {
    if (c->off > c->len || 2 > c->len - c->off) return -1;
    *v = (uint16_t)(c->p[c->off] | (c->p[c->off + 1] << 8));
    c->off += 2;
    return 0;
}
/* Is this a model-relative path we are willing to create on our own disk?
 *
 * File names arrive from the network — a tracker's placement, a peer's
 * manifest, a slice assignment — and are then joined onto a local directory
 * and opened with O_CREAT. Without this check a hostile peer (or a hostile
 * tracker on an unsigned swarm) names a file `../../.bashrc`, and the victim
 * dutifully creates and ftruncates it before a single byte of model has been
 * verified. Signatures do not help: the mirror file is made before any hash
 * is fetched, and the attacker picks the name.
 *
 * So: relative, no `..` component anywhere, no empty or `.` components, no
 * backslashes (a Windows port would treat them as separators), nothing below
 * space or above ASCII. Deliberately narrow — model directories are boring,
 * and anything interesting here is an attack. */
static LMB_MAYBE_UNUSED int lmb_rel_ok(const char *rel) {
    if (!rel || !rel[0] || rel[0] == '/' || strlen(rel) >= LMB_PATH_MAX) return 0;
    const char *p = rel;
    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') {
            if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == '\\')
                return 0;
            p++;
        }
        size_t n = (size_t)(p - seg);
        if (n == 0) return 0;                                  /* "" or "//" */
        if (n == 1 && seg[0] == '.') return 0;                 /* "." */
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return 0; /* ".." */
        if (*p == '/') p++;
    }
    return 1;
}

/* Numeric compatibility is stricter than an engine family name. Different
 * source trees, compilers, ISA paths or fast-math settings may all produce a
 * different last bit while still accepting the same tensors. Official builds
 * inject a stable source hash; the rest is derived from compiler macros so an
 * EMANIFEST comparison is cheap and deterministic. */
#ifndef LMBE_ENGINE_ID
#define LMBE_ENGINE_ID "unknown"
#endif
#ifndef LMBE_SOURCE_ID
#define LMBE_SOURCE_ID "unversioned"
#endif
#define LMB_STR_INNER(x) #x
#define LMB_STR(x) LMB_STR_INNER(x)
#ifdef _OPENMP
#define LMB_PROFILE_OMP LMB_STR(_OPENMP)
#else
#define LMB_PROFILE_OMP "none"
#endif
#ifdef __FAST_MATH__
#define LMB_PROFILE_MATH "fast"
#else
#define LMB_PROFILE_MATH "strict"
#endif
#if defined(__AVX512BF16__)
#define LMB_PROFILE_ISA "avx512bf16"
#elif defined(__AVX512VNNI__)
#define LMB_PROFILE_ISA "avx512vnni"
#elif defined(__AVX512F__)
#define LMB_PROFILE_ISA "avx512f"
#elif defined(__AVXVNNI__)
#define LMB_PROFILE_ISA "avxvnni"
#elif defined(__AVX2__)
#define LMB_PROFILE_ISA "avx2"
#elif defined(__ARM_FEATURE_MATMUL_INT8)
#define LMB_PROFILE_ISA "arm-i8mm"
#elif defined(__ARM_FEATURE_DOTPROD)
#define LMB_PROFILE_ISA "arm-dotprod"
#elif defined(__aarch64__)
#define LMB_PROFILE_ISA "aarch64"
#elif defined(__x86_64__)
#define LMB_PROFILE_ISA "x86_64"
#else
#define LMB_PROFILE_ISA "generic"
#endif
#ifdef __VERSION__
#define LMB_PROFILE_CC __VERSION__
#else
#define LMB_PROFILE_CC "unknown-cc"
#endif

static LMB_MAYBE_UNUSED void lmb_build_profile(char *out, size_t cap) {
    snprintf(out, cap, "abi=2;engine=%s;src=%s;cc=%s;isa=%s;omp=%s;math=%s;f32=%zu",
             LMBE_ENGINE_ID, LMBE_SOURCE_ID, LMB_PROFILE_CC, LMB_PROFILE_ISA,
             LMB_PROFILE_OMP, LMB_PROFILE_MATH, sizeof(float));
}

/* Name the first `key=val` segment of two build profiles that differs, so an
 * "incompatible manifest" points at the one knob to match — nearly always a
 * different compiler version (cc=) or -march tier (isa=) between the two
 * machines, or a src= from a different engine checkout — instead of a blank
 * "build". Writes "<key>: peer='<a>' vs local='<b>'"; falls back to the whole
 * strings if they differ only in structure. */
static LMB_MAYBE_UNUSED void lmb_profile_diff(const char *peer, const char *local,
                                              char *out, size_t cap) {
    const char *a = peer, *b = local;
    while (*a && *b) {
        const char *ae = a; while (*ae && *ae != ';') ae++;
        const char *be = b; while (*be && *be != ';') be++;
        size_t an = (size_t)(ae - a), bn = (size_t)(be - b);
        if (an != bn || memcmp(a, b, an)) {
            size_t kn = 0;                          /* key is up to '=' */
            while (kn < an && a[kn] != '=') kn++;
            const char *av = kn < an ? a + kn + 1 : a;   /* value after '=' */
            int avn = kn < an ? (int)(an - kn - 1) : (int)an;
            size_t lk = 0;
            while (lk < bn && b[lk] != '=') lk++;
            const char *bv = lk < bn ? b + lk + 1 : b;
            int bvn = lk < bn ? (int)(bn - lk - 1) : (int)bn;
            snprintf(out, cap, "%.*s: peer='%.*s' vs local='%.*s'",
                     (int)kn, a, avn, av, bvn, bv);
            return;
        }
        a = *ae ? ae + 1 : ae;
        b = *be ? be + 1 : be;
    }
    if (strcmp(peer, local))
        snprintf(out, cap, "peer='%s' vs local='%s'", peer, local);
    else
        snprintf(out, cap, "identical");
}

/* Value of one `key=` segment of a build profile, "" if absent. */
static LMB_MAYBE_UNUSED int lmb_prof_field(const char *p, const char *key,
                                           char *out, size_t cap) {
    size_t klen = strlen(key);
    for (const char *s = p; *s; ) {
        if (!strncmp(s, key, klen) && s[klen] == '=') {
            const char *v = s + klen + 1, *e = v;
            while (*e && *e != ';') e++;
            size_t n = (size_t)(e - v);
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n); out[n] = 0;
            return 0;
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
    }
    if (cap) out[0] = 0;
    return -1;
}

/* Compare two build profiles field by field, three tiers:
 *  - identity/ABI (abi, engine, src, math, f32): a mismatch means the peer runs
 *    different code or a different model — always incompatible.
 *  - codegen (cc, isa): the peer may round the low bits differently. Incompatible
 *    by default — byte-identity is the guarantee — but with allow_codegen it is
 *    downgraded to a warning so a heterogeneous swarm is possible for callers
 *    that accept approximate results and pair it with spot-check verification.
 *  - omp: the OpenMP *version* does not change results on its own — the thread
 *    count and reduction order do, and neither is in the profile — so it never
 *    gates (two libgomp versions no longer refuse each other).
 * Returns 0 compatible, 1 incompatible (why = the differing field), 2 compatible
 * with a codegen warning (why = the field that may round differently). */
/* Which differences refuse a peer and which admit it under verification.
 *
 * abi/engine/f32 stay hard: a different wire ABI, engine family or float
 * width produces garbage-shaped activations — no verification can save that.
 * src/math moved OUT of the hard set: a peer built from a different engine
 * checkout (or math mode) computes the same shapes and is exactly as
 * checkable as one built by a different compiler, and refusing it was the
 * single biggest reason real users could not join a swarm — every machine
 * had to hold byte-identical colibri+lumabri checkouts AND the same gcc.
 * Now they are admitted like cc/isa: a warning, spot-check verification,
 * and LMB strict mode (`LUMABRI_STRICT=1`) for the bit-identity purist —
 * which restores the refusal for all five. */
static LMB_MAYBE_UNUSED int lmb_profile_compat(const char *peer, const char *local,
                                               int allow_codegen,
                                               char *why, size_t cap) {
    static const char *const hard[] = { "abi", "engine", "f32" };
    static const char *const codegen[] = { "src", "math", "cc", "isa" };
    char a[72], b[72];   /* a profile value; longest in practice is cc= (~40) */
    for (size_t i = 0; i < sizeof hard / sizeof *hard; i++) {
        lmb_prof_field(peer, hard[i], a, sizeof a);
        lmb_prof_field(local, hard[i], b, sizeof b);
        if (strcmp(a, b)) {
            snprintf(why, cap, "%s: peer='%s' vs local='%s'", hard[i], a, b);
            return 1;
        }
    }
    int warned = 0;
    for (size_t i = 0; i < sizeof codegen / sizeof *codegen; i++) {
        lmb_prof_field(peer, codegen[i], a, sizeof a);
        lmb_prof_field(local, codegen[i], b, sizeof b);
        if (strcmp(a, b)) {
            if (!allow_codegen) {
                snprintf(why, cap, "%s: peer='%s' vs local='%s'", codegen[i], a, b);
                return 1;
            }
            if (!warned) {
                snprintf(why, cap, "%s: peer='%s' vs local='%s'", codegen[i], a, b);
                warned = 1;
            }
        }
    }
    return warned ? 2 : 0;   /* 0: differ only in omp (ignored) */
}

static int lmb_cur_u32(LmbCur *c, uint32_t *v) {
    if (c->off > c->len || 4 > c->len - c->off) return -1;
    *v = lmb_get32(c->p + c->off); c->off += 4;
    return 0;
}
static LMB_MAYBE_UNUSED int lmb_cur_u64(LmbCur *c, uint64_t *v) {
    uint32_t lo, hi;
    if (lmb_cur_u32(c, &lo) || lmb_cur_u32(c, &hi)) return -1;
    *v = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}
static int lmb_cur_bytes(LmbCur *c, void *dst, size_t n) {
    if (c->off > c->len || n > c->len - c->off) return -1;
    memcpy(dst, c->p + c->off, n); c->off += n;
    return 0;
}
static int lmb_cur_str(LmbCur *c, char *dst, size_t cap) {
    uint16_t n;
    if (lmb_cur_u16(c, &n)) return -1;
    if (c->off > c->len || n > c->len - c->off || (size_t)n + 1 > cap) return -1;
    memcpy(dst, c->p + c->off, n); dst[n] = 0; c->off += n;
    return 0;
}

/* ---- sockets ----------------------------------------------------------- */

static int lmb_env_int(const char *name, int fallback, int lo, int hi) {
    const char *s = getenv(name);
    if (!s || !*s) return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < lo || v > hi) return fallback;
    return (int)v;
}

static void lmb_set_io_timeout(int fd, int timeout_ms) {
    if (timeout_ms <= 0) return;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* Thread-per-connection keeps persistent pooled sockets simple, but it must
 * have an admission boundary. This small atomic gate is shared by tracker,
 * byte peers and expert peers; excess connections are closed before a stack
 * or request buffer is allocated. */
typedef struct { _Atomic unsigned active; unsigned limit; } LmbConnGate;
#define LMB_CONN_GATE_INIT { .active = 0, .limit = LMB_DEFAULT_MAX_CONNECTIONS }

static LMB_MAYBE_UNUSED void lmb_conn_gate_init(LmbConnGate *g) {
    g->limit = (unsigned)lmb_env_int("LUMABRI_MAX_CONNECTIONS",
                                     (int)LMB_DEFAULT_MAX_CONNECTIONS, 1, 65536);
}

static LMB_MAYBE_UNUSED int lmb_conn_gate_enter(LmbConnGate *g) {
    unsigned n = atomic_load(&g->active);
    while (n < g->limit) {
        if (atomic_compare_exchange_weak(&g->active, &n, n + 1)) return 1;
    }
    return 0;
}

static LMB_MAYBE_UNUSED void lmb_conn_gate_leave(LmbConnGate *g) {
    atomic_fetch_sub(&g->active, 1);
}

/* Why the most recent lmb_connect_ms in this translation unit failed. A
 * filtered port and a dead process both surface as "cannot connect", and an
 * operator needs opposite actions for them: one is a firewall rule, the other
 * is a process that is not running. Only the value read immediately after a
 * failed connect is meaningful. */
static LMB_MAYBE_UNUSED int lmb_connect_errno;

static LMB_MAYBE_UNUSED const char *lmb_connect_why(void) {
    switch (lmb_connect_errno) {
    case 0:            return "unreachable";
    case ETIMEDOUT:    return "no reply before the timeout — the port is "
                              "filtered or the host is unreachable (firewall?)";
    case ECONNREFUSED: return "connection refused — nothing is listening there";
    case EHOSTUNREACH: return "no route to the host";
    case ENETUNREACH:  return "no route to that network";
    default:           return strerror(lmb_connect_errno);
    }
}

/* addr is "host:port". Bounded connect: an unreachable peer must cost
 * `timeout_ms`, not the kernel's minutes — the caller has a relay to fall
 * back to. Returns a blocking fd with TCP_NODELAY, or -1. */
static int lmb_connect_ms_io(const char *addr, int timeout_ms, int io_timeout_ms) {
    char host[256];
    lmb_connect_errno = 0;
    const char *colon = strrchr(addr, ':');
    if (!colon || (size_t)(colon - addr) >= sizeof host) return -1;
    memcpy(host, addr, (size_t)(colon - addr)); host[colon - addr] = 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res)) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            int err = -1;
            socklen_t el = sizeof err;
            int ready = poll(&pf, 1, timeout_ms);
            if (ready > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                r = 0;
            else
                lmb_connect_errno = ready == 0 ? ETIMEDOUT
                                   : (err > 0 ? err : errno);
        } else if (r < 0) {
            lmb_connect_errno = errno;
        }
        if (r == 0) { fcntl(fd, F_SETFL, fl); lmb_connect_errno = 0; break; }
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        lmb_set_io_timeout(fd, io_timeout_ms);
        /* when this component has enabled encryption, every outbound
         * connection handshakes here, before any frame is sent */
        if (lmb_enc_wrap && lmb_enc_wrap(fd, 1, addr)) { lmb_close(fd); fd = -1; }
    }
    return fd;
}

static int lmb_connect_ms(const char *addr, int timeout_ms) {
    return lmb_connect_ms_io(addr, timeout_ms,
                            lmb_env_int("LUMABRI_IO_TIMEOUT_MS", LMB_DEFAULT_IO_TIMEOUT_MS,
                                        100, 3600000));
}
static int lmb_connect(const char *addr) { return lmb_connect_ms(addr, 5000); }

/* ---- userspace network emulation ---------------------------------------
 * Measuring what a LAN or a WAN would cost normally means a netem qdisc,
 * which needs root and changes the host's networking for every process.
 * This does the same job inside a serving peer: it holds the reply for the
 * configured round-trip time before sending it. Because the K requests of a
 * layer travel on K sockets served by K threads, the waits overlap exactly
 * as real flight time would. Zero cost when the variables are unset.
 *
 *   LUMABRI_RTT_US=250 LUMABRI_JITTER_US=50                 ≈ gigabit LAN
 *   LUMABRI_RTT_US=30000 LUMABRI_JITTER_US=5000 LUMABRI_LOSS_PPM=1000 ≈ WAN
 *
 * Loss is approximated the only honest way available in userspace: by
 * paying a retransmission timeout (LUMABRI_RTO_US) on that fraction of
 * replies. Applied to PING too, on purpose: probes must see the emulated
 * distance, or proximity-aware peer selection could not be tested. */

static long lmb_emu_rtt_us = -1, lmb_emu_jitter_us, lmb_emu_loss_ppm, lmb_emu_rto_us;
static __thread unsigned lmb_emu_seed;

static inline void lmb_emu_parse(void) {
    if (lmb_emu_rtt_us >= 0) return;       /* benign race: the parse is idempotent */
    const char *e;
    lmb_emu_jitter_us = (e = getenv("LUMABRI_JITTER_US")) ? atol(e) : 0;
    lmb_emu_loss_ppm  = (e = getenv("LUMABRI_LOSS_PPM"))  ? atol(e) : 0;
    lmb_emu_rto_us    = (e = getenv("LUMABRI_RTO_US"))    ? atol(e) : 200000;
    lmb_emu_rtt_us    = (e = getenv("LUMABRI_RTT_US"))    ? atol(e) : 0;
}

static inline void lmb_emu_delay(void) {
    lmb_emu_parse();
    if (!lmb_emu_rtt_us && !lmb_emu_jitter_us && !lmb_emu_loss_ppm) return;
    if (!lmb_emu_seed) lmb_emu_seed = (unsigned)(uintptr_t)&lmb_emu_seed | 1u;
    long us = lmb_emu_rtt_us;
    if (lmb_emu_jitter_us > 0) {
        lmb_emu_seed = lmb_emu_seed * 1664525u + 1013904223u;
        us += (long)(lmb_emu_seed % (unsigned)(2 * lmb_emu_jitter_us + 1)) - lmb_emu_jitter_us;
    }
    if (lmb_emu_loss_ppm > 0) {
        lmb_emu_seed = lmb_emu_seed * 1664525u + 1013904223u;
        if (lmb_emu_seed % 1000000u < (unsigned)lmb_emu_loss_ppm) us += lmb_emu_rto_us;
    }
    if (us <= 0) return;
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
}

static inline int lmb_emu_active(void) {
    lmb_emu_parse();
    return lmb_emu_rtt_us > 0 || lmb_emu_jitter_us > 0 || lmb_emu_loss_ppm > 0;
}

static LMB_MAYBE_UNUSED int lmb_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 64)) {
        close(fd); return -1;
    }
    return fd;
}

/* Sends AUTH if LUMABRI_TOKEN is set. Call right after connecting to a
 * tracker. Returns 0 (also when no token is configured). */
static int lmb_auth(int fd) {
    const char *tok = getenv("LUMABRI_TOKEN");
    if (!tok || !tok[0]) return 0;
    if (strlen(tok) > LMB_TOKEN_MAX) { errno = EMSGSIZE; return -1; }
    LmbBuf b = {0};
    if (lmb_buf_str(&b, tok)) { free(b.p); return -1; }
    int rc = lmb_send(fd, LMB_AUTH, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    if (rc) return rc;
    LmbMsg m = {0};
    rc = lmb_recv(fd, &m);
    int ok = rc == 0 && m.op == LMB_OK;
    lmb_msg_free(&m);
    return ok ? 0 : -1;
}

/* One-shot request/response on a fresh connection (tracker traffic; bulk
 * reads use pooled persistent connections instead). Returns 0 and fills
 * `resp` (caller frees), or -1. */
static int lmb_request_pay(const char *addr, uint32_t op,
                           const void *body, uint32_t body_len,
                           const void *pay, uint32_t pay_len, LmbMsg *resp) {
    int fd = lmb_connect(addr);
    if (fd < 0) return -1;
    int rc = lmb_auth(fd);
    if (rc == 0) rc = lmb_send(fd, op, body, body_len, pay, pay_len);
    if (rc == 0) rc = lmb_recv(fd, resp);
    lmb_close(fd);
    return rc;
}

static int lmb_request(const char *addr, uint32_t op,
                       const void *body, uint32_t body_len, LmbMsg *resp) {
    return lmb_request_pay(addr, op, body, body_len, NULL, 0, resp);
}

/* Fetch the operator-bound identity of a complete model. Signature checking
 * stays with the caller because only the caller owns the public key. */
static LMB_MAYBE_UNUSED int lmb_model_identity_get(const char *tracker,
                                                   const char *model,
                                                   LmbModelIdentity *id) {
    memset(id, 0, sizeof *id);
    if (!tracker || !*tracker || !model || !*model) return -1;
    LmbBuf b = {0};
    if (lmb_buf_str(&b, model)) return -1;
    LmbMsg m = {0};
    int rc = lmb_request(tracker, LMB_MODEL_ID, b.p, (uint32_t)b.len, &m);
    free(b.p);
    if (rc || m.op != LMB_MODEL_ID_R) { lmb_msg_free(&m); return -1; }
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t magic = 0, has_sig = 0;
    int bad = lmb_cur_u32(&c, &magic) || magic != LMB_MODEL_ID_MAGIC ||
              lmb_cur_str(&c, id->model, sizeof id->model) ||
              lmb_cur_bytes(&c, id->root, sizeof id->root) ||
              lmb_cur_u32(&c, &has_sig) || has_sig > 1 ||
              (has_sig && lmb_cur_bytes(&c, id->sig, sizeof id->sig)) ||
              c.off != c.len;
    id->has_sig = has_sig != 0;
    lmb_msg_free(&m);
    if (bad || strcmp(id->model, model)) { memset(id, 0, sizeof *id); return -1; }
    return 0;
}

#endif /* LUMABRI_PROTO_H */
