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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LMB_MAGIC     0x31424D4Cu           /* "LMB1" read as little-endian */
#define LMB_MAX_BODY  (64u << 20)   /* a REGISTER carrying block hashes for a
                                       whole huge model must fit: 32 B/MiB */
#define LMB_MAX_PAY   (64u << 20)
/* Rows an EXEC may carry. A real batch is a prompt's worth of positions, so
 * this is generous; it exists so a length can never be chosen to overflow
 * the size arithmetic on a model with a large hidden dimension. */
#define LMB_MAX_EXEC_ROWS 4096u
#define LMB_PATH_MAX  512
#define LMB_HASH_CHUNK (1u << 20)   /* integrity granularity: sha256 per MiB */
#define LMB_HASH_MAGIC 0x48414853u  /* "SHAH": optional hash section marker */

enum {
    LMB_PING = 1, LMB_OK = 2, LMB_ERR = 3,
    LMB_MANIFEST = 4, LMB_MANIFEST_R = 5,
    LMB_READ = 6, LMB_READ_R = 7,
    LMB_REGISTER = 8, LMB_PLACEMENT = 9, LMB_PLACEMENT_R = 10,
    /* phase 2 — the peer EXECUTES the expert instead of shipping its bytes:
     * EXEC carries one activation row, EXEC_R carries the expert's output.
     * EMANIFEST asks which (layer,expert) pairs a node actually holds. */
    LMB_EXEC = 11, LMB_EXEC_R = 12,
    LMB_EMANIFEST = 13, LMB_EMANIFEST_R = 14,
    /* SWARM: anonymous network status from the tracker. No names, no
     * addresses in the reply — per peer only: model, bytes held, bytes
     * served, reads served, seconds since last heartbeat. */
    LMB_SWARM = 15, LMB_SWARM_R = 16,
    /* Relay, the NAT-survival floor: a maintainer that cannot accept inbound
     * connections still serves, through the tracker. RREAD goes chatter →
     * tracker {model,path,off,len}; the tracker forwards RREAD_FWD {id,...}
     * down the maintainer's persistent control connection (the same one that
     * heartbeats), the maintainer answers RREAD_R {id} + pay, the tracker
     * routes the payload back. Direct peer-to-peer stays the first choice;
     * the relay is the fallback that makes zero router configuration work.
     * QUIC + hole punching later removes the relay from the path; it cannot
     * remove the need for this floor (symmetric NATs defeat punching too). */
    LMB_RREAD = 17, LMB_RREAD_FWD = 18, LMB_RREAD_R = 19,
    /* AUTH: private swarms. When the tracker runs with --token, the first
     * frame on every connection must be AUTH {str token}; everything else on
     * an unauthenticated connection is refused. Clients send it whenever
     * LUMABRI_TOKEN is set. */
    LMB_AUTH = 20,
    /* ASSIGN: "the server decides". A joining maintainer offers a byte
     * budget and the files it already holds; the tracker answers with the
     * slice it should pull and serve, rarest-first — the least-replicated
     * files of the model come first, so every new donor heals the swarm
     * where it is thinnest. body: {str model, u64 budget, u32 n, n×str path}
     * reply: {u32 n, n×{str path, u64 size}} */
    LMB_ASSIGN = 21, LMB_ASSIGN_R = 22,
    /* Phase-2 discovery — the bootstrap-and-delegate policy. An expert node
     * advertises itself with EREG {name, addr, model, u32 nexperts} on a
     * heartbeat, exactly like a maintainer REGISTERs; a chatter asks
     * EPEERS {model} and gets the live expert-capable addresses back. The
     * machine that `lumabri serve`s a model always runs one expert node, so
     * a brand-new swarm works from minute zero with the server executing
     * every expert; donors that join later are discovered and win the calls
     * they are nearest for; a donor that dies fails over — ultimately back
     * to the server, which holds everything. */
    LMB_EREG = 23, LMB_EPEERS = 24, LMB_EPEERS_R = 25,
    /* Integrity for the open swarm. A registering maintainer appends a
     * sha256 per LMB_HASH_CHUNK of every file it holds; the tracker keeps
     * the FIRST announcement of each (model, path) as ground truth — the
     * origin server registers before any donor exists — and rejects the
     * files of any later registrant whose hashes disagree (poison dies at
     * the index). HASHES {model, path} → HASHES_R {str model, u32 chunk,
     * u32 n, u64 size, u32 has_sig, [64 sig]} + pay (n × 32 raw bytes) hands
     * the truth to chatters and pulling donors,
     * which verify every fetched block against it: a lying peer's bytes
     * are rejected and refetched elsewhere, loudly. The root of trust is
     * the swarm operator (tracker + origin), never the peers. The reply
     * carries model and size because the signature is over all of it: a
     * verifier must be able to rebuild the signed message itself, and a
     * courier that could not supply the fields could rewrite them. Keep this
     * comment in step with the encoder — it went stale once and the donor's
     * decoder went stale with it, reading the model string as a chunk size
     * and calling the whole record malformed. */
    LMB_HASHES = 26, LMB_HASHES_R = 27,
    /* "the server decides", for compute. A donor of disk already gets its
     * slice assigned rarest-first; a donor of COMPUTE had to be told which
     * experts to hold by hand (--stride 9:3), which means knowing how many
     * other donors exist and which index is free — coordination the swarm
     * was supposed to remove. EASSIGN {model, slots, n_experts, capacity,
     * u32 mask_len, routed mask, u32 have_len, held bitmap} asks the tracker
     * instead; the reply {u32 n, n×{u32 layer, u32 eid}} is the set to hold,
     * least-replicated first, keeping whatever the node already has so a
     * restart does not re-download. A node that passes --layers/--stride
     * opts out and is simply counted like any other replica. */
    LMB_EASSIGN = 28, LMB_EASSIGN_R = 29,
};

/* REGISTER body: str name, str addr, str model, u64 held_bytes,
 *                u64 served_bytes, u64 served_reads, u32 n { str path, u64 size }
 * PLACEMENT body: empty = every model; str model = that model's files only. */

typedef struct {
    uint32_t op;
    uint8_t *body; uint32_t body_len;
    uint8_t *pay;  uint32_t pay_len;
} LmbMsg;

/* ---- full-buffer socket I/O ------------------------------------------- */

static int lmb_write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int lmb_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;              /* peer closed mid-frame */
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* ---- frames ------------------------------------------------------------ */

static void lmb_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t lmb_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int lmb_send(int fd, uint32_t op, const void *body, uint32_t body_len,
                    const void *pay, uint32_t pay_len) {
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
    uint8_t pre[16];
    memset(m, 0, sizeof *m);
    if (lmb_read_full(fd, pre, 16)) return -1;
    if (lmb_get32(pre) != LMB_MAGIC) return -1;
    m->op = lmb_get32(pre + 4);
    m->body_len = lmb_get32(pre + 8);
    m->pay_len = lmb_get32(pre + 12);
    if (m->body_len > LMB_MAX_BODY || m->pay_len > LMB_MAX_PAY) return -1;
    if (m->body_len) {
        if (!(m->body = (uint8_t *)malloc(m->body_len))) return -1;
        if (lmb_read_full(fd, m->body, m->body_len)) { free(m->body); m->body = NULL; return -1; }
    }
    if (m->pay_len) {
        if (!(m->pay = (uint8_t *)malloc(m->pay_len))) { free(m->body); m->body = NULL; return -1; }
        if (lmb_read_full(fd, m->pay, m->pay_len)) {
            free(m->body); free(m->pay); m->body = m->pay = NULL; return -1;
        }
    }
    return 0;
}

static void lmb_msg_free(LmbMsg *m) {
    free(m->body); free(m->pay);
    m->body = m->pay = NULL;
}

/* ---- body builder / cursor -------------------------------------------- */

typedef struct { uint8_t *p; size_t len, cap; } LmbBuf;

static int lmb_buf_reserve(LmbBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) cap *= 2;
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
static int lmb_buf_u64(LmbBuf *b, uint64_t v) {
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

typedef struct { const uint8_t *p; size_t len, off; } LmbCur;

static int lmb_cur_u16(LmbCur *c, uint16_t *v) {
    if (c->off + 2 > c->len) return -1;
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
static int lmb_rel_ok(const char *rel) {
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

static int lmb_cur_u32(LmbCur *c, uint32_t *v) {
    if (c->off + 4 > c->len) return -1;
    *v = lmb_get32(c->p + c->off); c->off += 4;
    return 0;
}
static int lmb_cur_u64(LmbCur *c, uint64_t *v) {
    uint32_t lo, hi;
    if (lmb_cur_u32(c, &lo) || lmb_cur_u32(c, &hi)) return -1;
    *v = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}
static int lmb_cur_bytes(LmbCur *c, void *dst, size_t n) {
    if (c->off + n > c->len) return -1;
    memcpy(dst, c->p + c->off, n); c->off += n;
    return 0;
}
static int lmb_cur_str(LmbCur *c, char *dst, size_t cap) {
    uint16_t n;
    if (lmb_cur_u16(c, &n)) return -1;
    if (c->off + n > c->len || (size_t)n + 1 > cap) return -1;
    memcpy(dst, c->p + c->off, n); dst[n] = 0; c->off += n;
    return 0;
}

/* ---- sockets ----------------------------------------------------------- */

/* addr is "host:port". Bounded connect: an unreachable peer must cost
 * `timeout_ms`, not the kernel's minutes — the caller has a relay to fall
 * back to. Returns a blocking fd with TCP_NODELAY, or -1. */
static int lmb_connect_ms(const char *addr, int timeout_ms) {
    char host[256];
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
            if (poll(&pf, 1, timeout_ms) > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                r = 0;
        }
        if (r == 0) { fcntl(fd, F_SETFL, fl); break; }
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
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

static int lmb_listen(int port) {
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
    LmbBuf b = {0};
    lmb_buf_str(&b, tok);
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
static int lmb_request(const char *addr, uint32_t op,
                       const void *body, uint32_t body_len, LmbMsg *resp) {
    int fd = lmb_connect(addr);
    if (fd < 0) return -1;
    int rc = lmb_auth(fd);
    if (rc == 0) rc = lmb_send(fd, op, body, body_len, NULL, 0);
    if (rc == 0) rc = lmb_recv(fd, resp);
    close(fd);
    return rc;
}

#endif /* LUMABRI_PROTO_H */
