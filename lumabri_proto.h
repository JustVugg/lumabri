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

/* Public helpers live in headers so every Lumabri binary stays a single
 * translation unit. A particular binary intentionally uses only a subset;
 * mark that contract explicitly without hiding warnings in ordinary .c code. */
#ifndef LMB_MAYBE_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define LMB_MAYBE_UNUSED __attribute__((unused))
#else
#define LMB_MAYBE_UNUSED
#endif
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0     /* platforms without it deliver SIGPIPE; the
                              daemons ignore the signal themselves */
#endif

#define LMB_MAGIC     0x31424D4Cu           /* "LMB1" read as little-endian */
#define LMB_MAX_BODY  (64u << 20)   /* a REGISTER carrying block hashes for a
                                       whole huge model must fit: 32 B/MiB */
#define LMB_MAX_PAY   (64u << 20)
#define LMB_MAX_CONTROL_BODY (4u << 20)
#define LMB_MAX_PLACEMENT_BODY (8u << 20) /* 4096 long paths, eight replicas */
#define LMB_MAX_SMALL_BODY   (64u << 10)
/* A DeepSeek prefill may queue dozens of cold experts behind one CPU gate;
 * 30 s incorrectly declared a healthy disk-backed node dead.  Connections
 * remain bounded separately, so five minutes is a safe default for bulk
 * work while deployments may lower it with LUMABRI_IO_TIMEOUT_MS. */
#define LMB_DEFAULT_IO_TIMEOUT_MS 300000
#define LMB_DEFAULT_MAX_CONNECTIONS 256u
#define LMB_EXPERT_MANIFEST_MAGIC 0x324D454Cu /* "LEM2": versioned EXEC manifest */
#define LMB_MODEL_ID_MAGIC         0x3144494Du /* "MID1": signed model identity */
#define LMB_MODEL_ROOT_LEN 32
#define LMB_BUILD_PROFILE_MAX 384
/* Rows an EXEC may carry. A real batch is a prompt's worth of positions, so
 * this is generous; it exists so a length can never be chosen to overflow
 * the size arithmetic on a model with a large hidden dimension. */
#define LMB_MAX_EXEC_ROWS 4096u
#define LMB_PATH_MAX  512
#define LMB_TOKEN_MAX 127u
#define LMB_HASH_CHUNK (1u << 20)   /* integrity granularity: sha256 per MiB */
#define LMB_HASH_MAGIC 0x48414853u  /* "SHAH": optional hash section marker */
#define LMB_PEER_AUTH_MAGIC 0x52554150u /* "PAUR": trailing peer-identity block */

/* Segment v2 is model-neutral: the opcodes carry versioned envelopes from
 * lumabri_segment.h, while model math and state stay behind the Colibri ABI.
 * No existing binary dispatches these operations until an adapter explicitly
 * opts in, so adding the wire vocabulary does not change local inference. */
#define LMB_SEG_V2_MAGIC          0x32474553u /* "SEG2" */
#define LMB_SEG_V2_VERSION        2u
#define LMB_SEG_ASSIGN_MAGIC      0x31415347u /* "GSA1" */
#define LMB_SEG_ASSIGN_VERSION    2u

/* Optional executor telemetry is negotiated on the existing EREG LMB_OK.
 * Every extension has its own magic, version, and bounded payload length so
 * old peers keep using the exact legacy body and new peers can skip versions
 * they do not understand without changing any opcode. */
#define LMB_EREG_CAP_MAGIC       0x50414345u /* "ECAP" */
#define LMB_EREG_CAP_VERSION     1u
#define LMB_EREG_CAP_STATS       0x00000001u
#define LMB_EREG_STATS_MAGIC     0x31545345u /* "EST1" */
#define LMB_EREG_STATS_VERSION   2u
#define LMB_EREG_STATS_LENGTH_V1 12u
#define LMB_EREG_STATS_LENGTH    40u
#define LMB_EXPERT_STATE_ASSIGNED 1u
#define LMB_EXPERT_STATE_LOADING  2u
#define LMB_EXPERT_STATE_RAM_READY 3u
#define LMB_EXPERT_STATE_VRAM_READY 4u
#define LMB_EXPERT_STATE_ACTIVE   5u
#define LMB_EXPERT_STATE_DRAINING 6u
#define LMB_EXPERT_RESIDENT_RAM   (1u << 0)
#define LMB_EXPERT_RESIDENT_VRAM  (1u << 1)
#define LMB_EXPERT_DISK_FALLBACK  (1u << 2)
#define LMB_SWARM_EXEC_MAGIC     0x31585753u /* "SWX1" */
#define LMB_SWARM_EXEC_VERSION   1u
#define LMB_SWARM_DETAIL_VERSION 2u
#define LMB_SWARM_ROLE_STORAGE   (1u << 0)
#define LMB_SWARM_ROLE_EXPERT    (1u << 1)
#define LMB_SWARM_ROLE_SEGMENT   (1u << 2)

/* Both arguments must point at the fixed LMB_TOKEN_MAX+1 authentication
 * buffers used by the daemons.  Compare the complete buffers so a remote
 * caller cannot learn a shared invite token one prefix at a time. */
static LMB_MAYBE_UNUSED int lmb_token_equal(const char a[LMB_TOKEN_MAX + 1],
                                            const char b[LMB_TOKEN_MAX + 1]) {
    unsigned diff = 0;
    for (size_t i = 0; i <= LMB_TOKEN_MAX; i++)
        diff |= (unsigned)(uint8_t)a[i] ^ (unsigned)(uint8_t)b[i];
    return diff == 0;
}

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
    /* A single signed identity for the complete model. The origin hashes the
     * canonical list of {path,size,block hashes}, signs that 32-byte root,
     * and the tracker only carries it. Expert nodes include the same record
     * in EMANIFEST, so a chatter cannot accidentally mix checkpoints. */
    LMB_MODEL_ID = 30, LMB_MODEL_ID_R = 31,
    /* NAT relay for compute. REXEC is {str model,u32 n_experts,EXEC body}
     * plus the activation payload. The tracker chooses a registered holder,
     * forwards {u32 id,EXEC body}+pay over its outbound EREG connection, and
     * returns the output. Direct EXEC remains preferred; this is the floor
     * for symmetric NATs and networks where no inbound port can be opened. */
    LMB_REXEC = 32, LMB_REXEC_FWD = 33, LMB_REXEC_R = 34,
    /* Relay coverage is separate from EPEERS because an unreachable/NATed
     * node has no direct EMANIFEST to query. Expert heartbeats append their
     * engine/build/shape metadata; ECOVER returns the OR of compatible live
     * holders' bitmaps, allowing the chatter to enable remote execution only
     * when every routed expert has either a direct or relayed path. */
    LMB_ECOVER = 35, LMB_ECOVER_R = 36,
    /* Peer identity. A control connection asks CHALLENGE and gets a 32-byte
     * random nonce; its REGISTER/EREG then carries {pubkey, sig} over that
     * nonce, so the tracker binds each name to the key that first claimed it
     * and refuses a different key under the same name. */
    LMB_CHALLENGE = 37, LMB_CHALLENGE_R = 38,

    /* Model-neutral Segment v2. Every state-mutating operation carries a
     * session id, request id, lease id, fencing epoch and route generation.
     * Snapshot/restore payloads are chunked; the 64 MiB frame cap therefore
     * never becomes a limit on the complete remote state. */
    LMB_SEG_OPEN = 39, LMB_SEG_OPEN_R = 40,
    LMB_SEG_RUN = 41, LMB_SEG_RUN_R = 42,
    LMB_SEG_SNAPSHOT = 43, LMB_SEG_SNAPSHOT_R = 44,
    LMB_SEG_RESTORE = 45, LMB_SEG_RESTORE_R = 46,
    LMB_SEG_CLOSE = 47, LMB_SEG_CLOSE_R = 48,
    LMB_SEG_HEALTH = 49, LMB_SEG_HEALTH_R = 50,

    /* Segment control plane. SREG is the signed, persistent heartbeat of a
     * range-native executor; its reply assigns the lease/fencing owner.
     * SEG_ROUTES is a model/schema/numeric compatibility query and returns a
     * generation-fenced immutable placement snapshot. Neither opcode is used
     * from the per-token or per-layer inference path. */
    LMB_SEG_REGISTER = 51, LMB_SEG_REGISTER_R = 52,
    LMB_SEG_ROUTES = 53, LMB_SEG_ROUTES_R = 54,
    /* A compute donor names the model/engine and receives one of the
     * origin's exact layer-aligned ranges, rarest first.  The subsequent
     * signed SEG_REGISTER remains the authority; this is only placement. */
    LMB_SEG_ASSIGN = 55, LMB_SEG_ASSIGN_R = 56,
    /* Stateful Segment relay. The caller names one exact registered peer and
     * wraps an ordinary Segment request. The tracker only forwards bytes over
     * that peer's signed outbound control connection; the executor remains
     * the sole authority for session, lease and request-id validation. */
    LMB_RSEG = 57, LMB_RSEG_FWD = 58, LMB_RSEG_R = 59,
    /* Human-facing observability. Unlike the legacy anonymous SWARM reply,
     * this endpoint returns the peer names chosen by their operators plus
     * role/load counters. It deliberately omits network addresses and public
     * keys: the TUI can explain who is doing work without leaking how to
     * reach a private donor. The body is versioned independently. */
    LMB_SWARM_DETAIL = 60, LMB_SWARM_DETAIL_R = 61,
    /* NAT probe: "dial me back at this port". The tracker connects to the
     * requester's OBSERVED address, so a caller can only ever probe itself. */
    LMB_REACH = 64, LMB_REACH_R = 65,
    /* A donor that cannot fit its assigned range releases the short-lived
     * placement promise immediately, so another READY-capable machine need
     * not wait for its timeout. This grants no lease or execution authority. */
    LMB_SEG_ASSIGN_RELEASE = 62,
    /* Targeted tunnel dialects, for peers that only the tracker can reach.
     * TEXEC carries one ordinary EXEC to ONE named holder and answers with
     * a plain LMB_EXEC_R, so a chatter can treat "peer behind the tracker"
     * exactly like any other replica: same sockets, same predictors, same
     * hedging, same spot-check. TMAN fetches that peer's expert manifest
     * the same way. */
    LMB_TEXEC = 66,
    LMB_TMAN = 67, LMB_TMAN_FWD = 68, LMB_TMAN_R = 69,
};

/* REGISTER body: str name, str addr, str model, u64 held_bytes,
 *                u64 served_bytes, u64 served_reads, u32 n { str path, u64 size }
 * PLACEMENT body: empty = every model; str model = that model's files only. */

typedef struct {
    uint32_t op;
    uint8_t *body; uint32_t body_len;
    uint8_t *pay;  uint32_t pay_len;
    /* Encrypted frames are decrypted into one allocation and body/pay point
     * inside it.  Plain frames keep the legacy two-allocation layout. */
    uint8_t *storage;
    uint32_t rx_reserved;
} LmbMsg;

typedef struct {
    char model[64];
    uint8_t root[LMB_MODEL_ROOT_LEN];
    uint8_t sig[64];
    int has_sig;
} LmbModelIdentity;

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

/* A frame's legal shape is determined before allocating it. REGISTER is the
 * only large-body request because it may carry every block hash of a huge
 * model. Bulk byte and activation frames may have a large payload but only a
 * small body. Thus no unknown or control op can reserve 64+64 MiB merely by
 * writing a hostile preamble. */
static void lmb_frame_caps(uint32_t op, uint32_t *body_cap, uint32_t *pay_cap) {
    *body_cap = LMB_MAX_SMALL_BODY;
    *pay_cap = 0;
    switch (op) {
    case LMB_REGISTER:
        *body_cap = LMB_MAX_BODY;
        break;
    case LMB_MANIFEST_R:
    case LMB_SWARM_R:
    case LMB_SWARM_DETAIL_R:
    case LMB_EPEERS_R:
    case LMB_EMANIFEST_R:
    case LMB_EASSIGN_R:
    case LMB_SEG_ROUTES_R:
        *body_cap = LMB_MAX_CONTROL_BODY;
        break;
    case LMB_PLACEMENT_R:
        *body_cap = LMB_MAX_PLACEMENT_BODY;
        break;
    case LMB_READ_R:
    case LMB_RREAD_R:
    case LMB_HASHES_R:
    case LMB_EXEC_R:
    case LMB_REXEC_R:
    case LMB_TEXEC:
    case LMB_TMAN_R:
    case LMB_SEG_RUN:
    case LMB_SEG_RUN_R:
    case LMB_SEG_SNAPSHOT_R:
    case LMB_SEG_RESTORE:
    case LMB_RSEG:
    case LMB_RSEG_FWD:
    case LMB_RSEG_R:
        *pay_cap = LMB_MAX_PAY;
        if (op == LMB_RSEG || op == LMB_RSEG_FWD || op == LMB_RSEG_R)
            *body_cap = LMB_MAX_CONTROL_BODY;
        break;
    case LMB_RREAD_FWD:
    case LMB_EXEC:
    case LMB_REXEC:
    case LMB_REXEC_FWD:
        *body_cap = LMB_MAX_CONTROL_BODY;
        *pay_cap = LMB_MAX_PAY;
        break;
    default:
        break;
    }
}

static int lmb_frame_shape_ok(uint32_t op, uint32_t body_len, uint32_t pay_len) {
    uint32_t bc, pc;
    lmb_frame_caps(op, &bc, &pc);
    return body_len <= bc && pay_len <= pc;
}

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

static int lmb_rx_reserve(LmbMsg *m, uint32_t n) {
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
    if (lmb_get32(pre) != LMB_MAGIC) return -1;
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
static int lmb_connect_ms(const char *addr, int timeout_ms) {
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
        lmb_set_io_timeout(fd, lmb_env_int("LUMABRI_IO_TIMEOUT_MS",
                                           LMB_DEFAULT_IO_TIMEOUT_MS, 100, 3600000));
        /* when this component has enabled encryption, every outbound
         * connection handshakes here, before any frame is sent */
        if (lmb_enc_wrap && lmb_enc_wrap(fd, 1, addr)) { lmb_close(fd); fd = -1; }
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
