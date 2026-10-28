/* Transport-independent wire constants, shapes and byte order.
 * The same definitions are used by POSIX and native Windows runtimes. */
#ifndef LUMABRI_WIRE_H
#define LUMABRI_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

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
    /* Where an executor's experts live. ERES_R body: u32 resident_flags
     * (LMB_EXPERT_RESIDENT_VRAM / _RAM / LMB_EXPERT_DISK_FALLBACK), u32 state,
     * u32 caps, then u32 hot_experts, u32 held_experts, u32 hot_permille.
     *
     * The flag alone cannot describe a node that keeps a RAM cache in front
     * of on-disk experts: holding a third of a model hot reads exactly like
     * holding none of it. The three counts say how much is hot, out of how
     * much, and how often a call is actually served without a disk read —
     * measured once enough calls have gone through, and an opening
     * estimate from the slot ratio before that. A chatter asks once after the
     * manifest; an older node answers ERR and is treated as "unknown", or
     * answers the first three fields alone and is read exactly as before.
     * Additive: the manifest itself is unchanged, so old chatters keep
     * admitting new nodes. */
    LMB_ERES = 70, LMB_ERES_R = 71,
    /* EXEC with an explicit encoding. Body: u32 layer, eid, D, nrows,
     * enc_in, then the router weights when the engine wants them applied
     * remotely (body_len == 20 + nrows*4); pay is nrows*D floats when
     * enc_in == 0, nrows*D bf16 halves when enc_in == 1. Reply body: u32
     * enc_out; pay likewise. A value travels as bf16 only when it IS a
     * bf16 (low 16 bits zero), so the arithmetic is unchanged: engines that
     * round their MoE input and expert output to bf16 (DeepSeek V4 does)
     * pay half the bytes, others keep floats, and nothing is approximated.
     * Advertised by an executor as bit 0 of the caps word ERES_R carries
     * third, after residency and state; an older node never receives EXEC2. */
    LMB_EXEC2 = 72, LMB_EXEC2_R = 73,
    /* An inference host: it owns the engine, the dense weights and the KV,
     * and a client sends it text. HELLO_R body: model_type, engine kind,
     * backend ("cpu"/"cuda"), u32 free sessions, u32 measured tok/s in
     * thousandths — zero meaning nobody has measured this host yet, which
     * is a state the screen shows rather than a number it invents.
     *
     * HOST_STREAM payloads carry chunks of the SUBMIT/DATA/DONE codec. When
     * transport encryption is enabled they remain authenticated and encrypted;
     * a local socketpair unwraps them so the terminal-facing Engine API still
     * sees an ordinary byte stream. */
    LMB_HOST_HELLO = 74, LMB_HOST_HELLO_R = 75,
    LMB_HOST_STREAM = 76,
    LMB_MACHINE_REPORT = 77, LMB_MACHINE_LIST = 78, LMB_MACHINE_LIST_R = 79,
    LMB_HOME_OFFER = 80, LMB_HOME_STATUS = 81, LMB_HOME_COMMIT = 82,
    LMB_HOME_START_HOST = 83, LMB_HOME_CANCEL = 84, LMB_HOME_PULSE = 85,
    LMB_HOME_DETACH = 86, /* close requester control; retain prepared weights */
    LMB_HOME_FEATURES = 87, LMB_HOME_HYBRID_ROUTES = 88, LMB_HOME_EXPERT = 89,
    LMB_HOME_QUERY = 90, LMB_HOME_RELEASE = 91, /* pinned requester, exact allocation + root */
    LMB_LINK_PROBE = 92, LMB_LINK_PROBE_R = 93, /* bounded authenticated echo */
    LMB_HOST_CONTROL = 94, LMB_HOST_CONTROL_R = 95, /* approved requester only; fenced drain/resume */
    LMB_HOME_NODE_CONTROL = 96, LMB_HOME_NODE_CONTROL_R = 97, /* keeper -> owned Segment channel */
    LMB_HOME_RETIRED_RELEASE = 98, LMB_HOME_RETIRED_RELEASE_R = 99, /* irreversible idle-node fence */
    /* Optional Segment RUN telemetry. GET on a separate connection is safe
     * against old trackers (ERR); PUT is accepted only from the registered
     * executor control connection. SGD1 discovery remains byte-for-byte v1. */
    LMB_SEG_COMPUTE_GET = 100, LMB_SEG_COMPUTE_GET_R = 101,
    LMB_SEG_COMPUTE_PUT = 102, LMB_SEG_COMPUTE_PUT_R = 103,
    LMB_HOME_EXIT_PROOF = 104, LMB_HOME_EXIT_PROOF_R = 105, /* immutable allocation, all descendants exited */
};
#define LMB_CAP_EXEC2 (1u << 0)
#define LMB_ENC_F32  0u
#define LMB_ENC_BF16 1u

/* bf16 on the wire, exact or not at all */
static LMB_MAYBE_UNUSED int lmb_bf16_exact(const float *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits; memcpy(&bits, &v[i], 4);
        if (bits & 0xFFFFu) return 0;
    }
    return 1;
}
static LMB_MAYBE_UNUSED void lmb_bf16_pack(uint16_t *dst, const float *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits; memcpy(&bits, &src[i], 4);
        dst[i] = (uint16_t)(bits >> 16);
    }
}
static LMB_MAYBE_UNUSED void lmb_bf16_unpack(float *dst, const uint16_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t bits = (uint32_t)src[i] << 16;
        memcpy(&dst[i], &bits, 4);
    }
}

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

static inline void lmb_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t lmb_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A frame's legal shape is determined before allocating it. REGISTER is the
 * only large-body request because it may carry every block hash of a huge
 * model. Bulk byte and activation frames may have a large payload but only a
 * small body. Thus no unknown or control op can reserve 64+64 MiB merely by
 * writing a hostile preamble. */
static inline void lmb_frame_caps(uint32_t op, uint32_t *body_cap, uint32_t *pay_cap) {
    *body_cap = LMB_MAX_SMALL_BODY;
    *pay_cap = 0;
    switch (op) {
    case LMB_LINK_PROBE:
    case LMB_LINK_PROBE_R:
        *body_cap = 16; *pay_cap = 64u << 10;
        break;
    case LMB_HOME_EXPERT:
        *body_cap = 80;
        *pay_cap = 256u << 10; /* One activation, at most 65536 float cells. */
        break;
    case LMB_REGISTER:
        *body_cap = LMB_MAX_BODY;
        break;
    case LMB_MANIFEST_R:
    case LMB_SWARM_R:
    case LMB_SWARM_DETAIL_R:
    case LMB_MACHINE_LIST_R:
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
    case LMB_EXEC2_R:
    case LMB_REXEC_R:
    case LMB_TEXEC:
    case LMB_TMAN_R:
    case LMB_SEG_RUN:
    case LMB_SEG_RUN_R:
    case LMB_SEG_SNAPSHOT_R:
    case LMB_SEG_RESTORE:
    case LMB_HOST_STREAM:
    case LMB_RSEG:
    case LMB_RSEG_FWD:
    case LMB_RSEG_R:
        *pay_cap = LMB_MAX_PAY;
        if (op == LMB_RSEG || op == LMB_RSEG_FWD || op == LMB_RSEG_R)
            *body_cap = LMB_MAX_CONTROL_BODY;
        break;
    case LMB_RREAD_FWD:
    case LMB_EXEC:
    case LMB_EXEC2:
    case LMB_REXEC:
    case LMB_REXEC_FWD:
        *body_cap = LMB_MAX_CONTROL_BODY;
        *pay_cap = LMB_MAX_PAY;
        break;
    default:
        break;
    }
}

static inline int lmb_frame_shape_ok(uint32_t op, uint32_t body_len, uint32_t pay_len) {
    uint32_t bc, pc;
    lmb_frame_caps(op, &bc, &pc);
    return body_len <= bc && pay_len <= pc;
}

#endif
