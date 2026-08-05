/* lumibri_proto.h — the lumibri wire protocol. Header-only, no dependencies.
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
#ifndef LUMIBRI_PROTO_H
#define LUMIBRI_PROTO_H

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LMB_MAGIC     0x31424D4Cu           /* "LMB1" read as little-endian */
#define LMB_MAX_BODY  (16u << 20)
#define LMB_MAX_PAY   (64u << 20)
#define LMB_PATH_MAX  512

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
static int lmb_cur_str(LmbCur *c, char *dst, size_t cap) {
    uint16_t n;
    if (lmb_cur_u16(c, &n)) return -1;
    if (c->off + n > c->len || (size_t)n + 1 > cap) return -1;
    memcpy(dst, c->p + c->off, n); dst[n] = 0; c->off += n;
    return 0;
}

/* ---- sockets ----------------------------------------------------------- */

/* addr is "host:port". Returns a connected fd with TCP_NODELAY, or -1. */
static int lmb_connect(const char *addr) {
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
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
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

/* One-shot request/response on a fresh connection (tracker traffic; bulk
 * reads use pooled persistent connections instead). Returns 0 and fills
 * `resp` (caller frees), or -1. */
static int lmb_request(const char *addr, uint32_t op,
                       const void *body, uint32_t body_len, LmbMsg *resp) {
    int fd = lmb_connect(addr);
    if (fd < 0) return -1;
    int rc = lmb_send(fd, op, body, body_len, NULL, 0);
    if (rc == 0) rc = lmb_recv(fd, resp);
    close(fd);
    return rc;
}

#endif /* LUMIBRI_PROTO_H */
