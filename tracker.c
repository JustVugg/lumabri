/* tracker.c — the Napster part of lumabri: an index of who holds what,
 * and — only when a direct connection fails — a relay for the bytes.
 *
 * Maintainers keep ONE persistent outbound control connection here: it
 * REGISTERs on connect and re-REGISTERs every few seconds as a heartbeat.
 * Chatters ask for a PLACEMENT (file → peers), for SWARM (anonymous status),
 * or — the NAT-survival floor — send an RREAD: the tracker forwards it down
 * the holding maintainer's control connection (RREAD_FWD), the maintainer
 * answers with the bytes (RREAD_R), and the tracker routes them back. A
 * maintainer behind any home NAT thus serves with zero router configuration;
 * direct peer-to-peer remains the first choice and the relay the fallback.
 *
 * Placement/liveness state is in memory and rebuilt from heartbeats after a
 * restart. Name-to-identity ownership is the exception: it is persisted
 * before admission, so restarting the tracker cannot reopen an honest name.
 *
 *   ./tracker [--port 7300]
 */
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>

#include "lumabri_proto.h"
#include "lumabri_segment_discovery.h"
#include "lumabri_sha.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"

#define MAX_PEERS  64
#define MAX_FILES  4096
#define STALE_S    30.0     /* silent this long → dropped from placements */
#define RELAY_WAIT_S 60     /* chatter-side cap on one relayed read */
#define RSEG_RATE_SLOTS 128

typedef struct { char path[LMB_PATH_MAX]; uint64_t size; } PFile;

typedef struct {
    char name[64], addr[64], model[64], source[64];
    char engine[64], profile[LMB_BUILD_PROFILE_MAX];
    uint32_t bits, hidden, slots, total_experts;
    uint64_t held_bytes, served_bytes, served_reads;
    uint64_t exec_calls;
    uint32_t exec_inflight;
    int have_exec_stats;
    uint32_t expert_state, expert_resident_flags, resident_experts;
    uint64_t expert_resident_bytes, expert_vram_bytes;
    double expert_ts;
    PFile *files; uint32_t nfiles;
    double ts;
    int used;
    unsigned refs;              /* control/relay users holding this slot */
    int is_expert;              /* compute identity (Expert and/or Segment) */
    int has_expert;             /* EREG capability is independently present */
    int has_segment;            /* SREG capability is independently present */
    uint8_t pubkey[32];         /* this name's identity, bound on first claim */
    int has_key;
    uint32_t nexperts;
    /* WHICH experts, not just how many: one bit per (slot, expert). Without
     * it the tracker can count executors but cannot tell whether they
     * overlap, and so cannot assign a newcomer the part nobody covers. Sent
     * on the EREG heartbeat; 2.4 KB for a 19456-expert model. */
    uint8_t *ebits; uint32_t enbits;
    LmbSegAdvert segment;
    LmbSegOwner segment_owner;
    double segment_ts;
    int segment_live;

    /* relay mailbox: one in-flight request per peer, serialized. The
     * chatter thread queues it and waits; the peer's control thread
     * forwards it and completes it. evfd wakes the control thread. */
    int ctrl_fd;                /* the live control connection, -1 if none */
    int evfd;
    pthread_mutex_t rq_lk;
    pthread_cond_t rq_cv;
    int rq_busy, rq_sent, rq_done, rq_ok;
    uint32_t rq_id, rq_next, rq_op, rq_inner_op;
    char rq_path[LMB_PATH_MAX];
    uint64_t rq_off; uint32_t rq_len;
    uint8_t *rq_body; uint32_t rq_body_len;
    uint8_t *rq_pay; uint32_t rq_pay_len;
    uint8_t *rq_resp; uint32_t rq_resp_len;
    uint8_t *rq_resp_pay; uint32_t rq_resp_pay_len;
    uint32_t rq_resp_op;
} Peer;

static Peer g_peers[MAX_PEERS];
typedef struct {
    char source[64];
    double tokens, updated;
    uint32_t in_flight;
} RsegRate;
static RsegRate g_rseg_rates[RSEG_RATE_SLOTS];
static pthread_mutex_t g_rseg_rate_lk = PTHREAD_MUTEX_INITIALIZER;
static int g_rseg_rate_per_s = 2048, g_rseg_burst = 4096;
static int g_rseg_source_concurrency = 32, g_rseg_queue_ms = 2000;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static int g_known_logged[MAX_PEERS];
static char g_token[LMB_TOKEN_MAX + 1]; /* --token: private swarm, invite required */
static LmbConnGate g_conn_gate = LMB_CONN_GATE_INIT;
static double g_stale_s = STALE_S;
static int g_max_names_per_source = 16;
static uint64_t g_segment_route_generation = 1;
static int g_segment_generation_ready;
static char g_bindings_path[512];

/* A range assignment is a short promise, like expert EASSIGN's promise: it
 * stops several donors booting concurrently from all selecting the same
 * rare slice before their heavyweight engines finish loading and register.
 * It is not a lease and grants no execution authority. */
#define MAX_SEGMENT_PROMISES 128
#define SEGMENT_PROMISE_TTL_S 300.0
typedef struct {
    int used;
    char model[LMB_SEG_MODEL_MAX];
    char name[LMB_SEG_PEER_NAME_MAX];
    char engine[LMB_SEG_ENGINE_MAX];
    uint8_t model_root[LMB_SEG_ROOT_BYTES];
    uint32_t begin, end;
    double ts;
} SegmentPromise;
static SegmentPromise g_segment_promises[MAX_SEGMENT_PROMISES];

/* Reserve one 32-bit generation epoch per tracker process. The durable high
 * half means a tracker restart can never publish a generation below a route
 * still cached by a chatter or held by a live executor. The low half is ample
 * for topology changes during one process lifetime and needs no fsync on each
 * heartbeat. Rolling trackers serialize epoch allocation with flock. */
static int segment_generation_init(void) {
    char path[sizeof g_bindings_path + 32];
    if (!g_bindings_path[0] ||
        snprintf(path, sizeof path, "%s.segment-generation", g_bindings_path) >=
            (int)sizeof path)
        return -1;
    int flags = O_RDWR | O_CREAT;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0 || flock(fd, LOCK_EX)) { if (fd >= 0) close(fd); return -1; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 022)) {
        flock(fd, LOCK_UN); close(fd); errno = EACCES; return -1;
    }
    char buf[64] = {0};
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    char *end = NULL;
    unsigned long long previous = 0;
    if (n < 0) { flock(fd, LOCK_UN); close(fd); return -1; }
    if (n) {
        errno = 0;
        previous = strtoull(buf, &end, 10);
        if (errno || end == buf || (*end && *end != '\n') ||
            previous >= UINT32_MAX) {
            flock(fd, LOCK_UN); close(fd); errno = EINVAL; return -1;
        }
    }
    uint32_t epoch = (uint32_t)previous + 1u;
    int out_len = snprintf(buf, sizeof buf, "%u\n", epoch);
    /* The decimal counter never shrinks when incremented, so overwrite first
     * and fsync without truncating the last valid epoch before the new one is
     * durable. */
    int bad = out_len <= 0 ||
              pwrite(fd, buf, (size_t)out_len, 0) != out_len || fsync(fd);
    flock(fd, LOCK_UN); close(fd);
    if (bad) return -1;
    g_segment_route_generation = (uint64_t)epoch << 32 | 1u;
    return 0;
}

static void segment_generation_bump(void) {
    /* Exhausting four billion topology changes without restarting is not a
     * useful operating state; fail closed at the reserved epoch boundary. */
    if ((uint32_t)g_segment_route_generation == UINT32_MAX) {
        fprintf(stderr, "[tracker] Segment generation epoch exhausted; "
                        "restart to reserve the next durable epoch\n");
        abort();
    }
    g_segment_route_generation++;
}

/* Unlike liveness/placement state, identity ownership must survive a tracker
 * restart.  Otherwise "TOFU" means only "trust until the process exits" and
 * an attacker can claim the honest name during the next boot. */
#define MAX_BINDINGS 4096
typedef struct { char name[64]; uint8_t pubkey[32]; int is_expert; } Binding;
static Binding g_bindings[MAX_BINDINGS];
static int g_nbindings;

static int binding_name_ok(const char *s) {
    if (!s || !*s || strlen(s) >= 64) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r' || *p < 0x20) return 0;
    return 1;
}

static int binding_find(const char *name, int is_expert) {
    for (int i = 0; i < g_nbindings; i++)
        if (g_bindings[i].is_expert == is_expert &&
            !strcmp(g_bindings[i].name, name)) return i;
    return -1;
}

static int binding_path_default(void) {
    if (g_bindings_path[0]) return 0;
    const char *e = getenv("LUMABRI_PEER_BINDINGS");
    if (e && *e) return snprintf(g_bindings_path, sizeof g_bindings_path, "%s", e)
                         < (int)sizeof g_bindings_path ? 0 : -1;
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    char dir[448];
    if (snprintf(dir, sizeof dir, "%s/.lumabri", home) >= (int)sizeof dir) return -1;
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    return snprintf(g_bindings_path, sizeof g_bindings_path,
                    "%s/tracker_peer_bindings", dir) < (int)sizeof g_bindings_path ? 0 : -1;
}

static int bindings_load(void) {
    if (binding_path_default()) return -1;
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(g_bindings_path, flags);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 022)) { close(fd); errno = EACCES; return -1; }
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return -1; }
    char line[768]; int rc = 0;
    while (fgets(line, sizeof line, fp)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (!a || !b || a != line + 1 || (line[0] != 'M' && line[0] != 'E')) { rc = -1; break; }
        *a++ = 0; *b++ = 0;
        if (strlen(a) != 64 || !binding_name_ok(b) || g_nbindings >= MAX_BINDINGS) { rc = -1; break; }
        Binding x = {0}; x.is_expert = line[0] == 'E';
        snprintf(x.name, sizeof x.name, "%s", b);
        if (lmb_unhex(x.pubkey, a, 32)) { rc = -1; break; }
        int old = binding_find(x.name, x.is_expert);
        if (old >= 0) {
            if (memcmp(g_bindings[old].pubkey, x.pubkey, 32)) { rc = -1; break; }
        } else g_bindings[g_nbindings++] = x;
    }
    if (ferror(fp)) rc = -1;
    fclose(fp);
    return rc;
}

/* Called under g_lk.  Durable publication precedes admission to the live
 * table, so a crash can lose liveness but never ownership. */
static int binding_check_or_add(const char *name, int is_expert,
                                const uint8_t pk[32]) {
    int old = binding_find(name, is_expert);
    if (old >= 0) return memcmp(g_bindings[old].pubkey, pk, 32) ? -1 : 0;
    if (!binding_name_ok(name) || g_nbindings >= MAX_BINDINGS) return -1;
    int flags = O_RDWR | O_CREAT;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(g_bindings_path, flags, 0600);
    if (fd < 0 || flock(fd, LOCK_EX)) { if (fd >= 0) close(fd); return -1; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 022)) {
        flock(fd, LOCK_UN); close(fd); errno = EACCES; return -1;
    }
    /* A second tracker may share the durable state during a rolling restart.
     * Re-read it while holding the file lock, so two processes cannot append
     * conflicting owners from stale in-memory snapshots. */
    int dfd = dup(fd), disk_found = 0, disk_bad = dfd < 0;
    FILE *rf = dfd >= 0 ? fdopen(dfd, "r") : NULL;
    if (!rf && dfd >= 0) { close(dfd); disk_bad = 1; }
    if (rf) {
        char line[768];
        while (fgets(line, sizeof line, rf)) {
            size_t n = strlen(line);
            if (n && line[n - 1] == '\n') line[--n] = 0;
            if (!n || line[0] == '#') continue;
            char kind = line[0], *a = strchr(line, '\t');
            char *b = a ? strchr(a + 1, '\t') : NULL;
            if (!a || !b || a != line + 1 || (kind != 'M' && kind != 'E')) {
                disk_bad = 1; break;
            }
            *a++ = 0; *b++ = 0;
            uint8_t saved[32];
            if (strlen(a) != 64 || !binding_name_ok(b) ||
                lmb_unhex(saved, a, sizeof saved)) { disk_bad = 1; break; }
            if ((kind == 'E') == is_expert && !strcmp(b, name)) {
                if (memcmp(saved, pk, sizeof saved)) { disk_bad = 1; break; }
                disk_found = 1;
            }
        }
        if (ferror(rf)) disk_bad = 1;
        fclose(rf);
    }
    if (disk_bad) { flock(fd, LOCK_UN); close(fd); return -1; }
    if (disk_found) {
        flock(fd, LOCK_UN); close(fd);
        Binding *x = &g_bindings[g_nbindings++];
        memset(x, 0, sizeof *x); x->is_expert = is_expert;
        snprintf(x->name, sizeof x->name, "%s", name); memcpy(x->pubkey, pk, 32);
        return 0;
    }
    char hex[65], row[768]; lmb_hex(hex, pk, 32);
    int rn = snprintf(row, sizeof row, "%c\t%s\t%s\n", is_expert ? 'E' : 'M', hex, name);
    int ok = rn > 0 && rn < (int)sizeof row;
    off_t end = lseek(fd, 0, SEEK_END);
    size_t off = 0;
    if (end < 0) ok = 0;
    while (ok && off < (size_t)rn) {
        ssize_t w = write(fd, row + off, (size_t)rn - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { ok = 0; break; }
        off += (size_t)w;
    }
    if (ok && fsync(fd)) ok = 0;
    flock(fd, LOCK_UN); close(fd);
    if (!ok) return -1;
    Binding *x = &g_bindings[g_nbindings++];
    memset(x, 0, sizeof *x); x->is_expert = is_expert;
    snprintf(x->name, sizeof x->name, "%s", name); memcpy(x->pubkey, pk, 32);
    return 0;
}

/* Ground truth: sha256 per LMB_HASH_CHUNK of every (model, path), taken
 * from the FIRST registrant — the origin server registers before any donor
 * exists. A later registrant whose hashes disagree is announcing poison:
 * that file is stripped from its offer and the lie is logged. */
typedef struct {
    char model[64], path[LMB_PATH_MAX];
    uint32_t nh;
    uint64_t size;
    uint8_t *hash;
    uint8_t sig[64]; int has_sig, sig_rank;
} GTruth;
static GTruth g_truth[MAX_FILES];
static int g_ntruth;
static LmbTrustKeys g_trust; /* --pubkey: one key or an overlap keyring */
#define g_have_pubkey (g_trust.n != 0)

#define MAX_MODELS 128
typedef struct { LmbModelIdentity id; int used, sig_rank; } GModel;
static GModel g_models[MAX_MODELS];

/* under g_lk. A first identity must describe exactly the hash inventory in
 * that registration; later partial donors may forward the already accepted
 * full root. Signed swarms additionally require the operator's signature. */
static int model_identity_check(const char *model, const LmbModelIdentity *id,
                                const PFile *files, uint32_t n,
                                uint8_t *const *hashes, const uint32_t *nh) {
    if (strcmp(model, id->model)) return 0;
    int sig_rank = -1;
    if (g_have_pubkey) {
        if (!id->has_sig) return 0;
        size_t ml = 0;
        uint8_t *msg = lmb_model_id_msg(model, id->root, &ml);
        if (msg) sig_rank = lmb_trust_match(&g_trust, id->sig, msg, ml);
        int ok = sig_rank >= 0;
        free(msg);
        if (!ok) return 0;
    }
    for (int i = 0; i < MAX_MODELS; i++)
        if (g_models[i].used && !strcmp(g_models[i].id.model, model)) {
            if (memcmp(g_models[i].id.root, id->root, 32)) return 0;
            if (g_models[i].id.has_sig && !id->has_sig) return 0;
            if ((!g_models[i].id.has_sig && id->has_sig) ||
                (id->has_sig && sig_rank > g_models[i].sig_rank)) {
                g_models[i].id = *id; g_models[i].sig_rank = sig_rank;
            }
            return 1;
        }
    LmbModelItem *items = (LmbModelItem *)calloc(n ? n : 1, sizeof *items);
    if (!items) return 0;
    for (uint32_t i = 0; i < n; i++) {
        /* Empty files legitimately have a zero-length hash vector. */
        if (nh[i] && !hashes[i]) { free(items); return 0; }
        items[i].path = files[i].path; items[i].size = files[i].size;
        items[i].nh = nh[i]; items[i].hashes = hashes[i];
    }
    uint8_t root[32];
    int ok = lmb_model_root(model, items, n, root) == 0 &&
             memcmp(root, id->root, 32) == 0;
    free(items);
    if (!ok) return 0;
    for (int i = 0; i < MAX_MODELS; i++)
        if (!g_models[i].used) {
            g_models[i].used = 1; g_models[i].id = *id;
            g_models[i].sig_rank = sig_rank;
            return 1;
        }
    return 0;
}

/* under g_lk; steals *hash on first sight. Returns 1 ok / 0 rejected.
 *
 * With --pubkey the tracker stops being an authority and becomes a witness:
 * a truth claim is accepted only if it carries the operator's signature,
 * so even a fully compromised tracker cannot invent bytes — it can refuse
 * to serve, but it cannot lie and be believed, because the chatter checks
 * the same signature independently. Without --pubkey the old first-come
 * rule applies (fine for a private swarm, not for strangers). */
static int truth_check(const char *model, const char *path, uint64_t size,
                       uint32_t nh, uint8_t **hash,
                       const uint8_t *sig, int has_sig) {
    uint64_t expected_nh = size / LMB_HASH_CHUNK +
                           (size % LMB_HASH_CHUNK != 0);
    if (expected_nh != nh || (nh && !*hash)) return 0;
    int sig_rank = -1;
    if (g_have_pubkey) {
        if (!has_sig) return 0;
        size_t mlen = 0;
        uint8_t *msg = lmb_truth_msg(model, path, LMB_HASH_CHUNK, size,
                                     *hash, nh, &mlen);
        if (!msg) return 0;
        sig_rank = lmb_trust_match(&g_trust, sig, msg, mlen);
        int ok = sig_rank >= 0;
        free(msg);
        if (!ok) return 0;
    }
    for (int i = 0; i < g_ntruth; i++)
        if (!strcmp(g_truth[i].model, model) && !strcmp(g_truth[i].path, path)) {
            if (g_truth[i].nh != nh || g_truth[i].size != size ||
                (nh && memcmp(g_truth[i].hash, *hash, (size_t)nh * 32))) return 0;
            if ((!g_truth[i].has_sig && has_sig) ||
                (has_sig && sig_rank > g_truth[i].sig_rank)) {
                memcpy(g_truth[i].sig, sig, 64);
                g_truth[i].has_sig = 1;
                g_truth[i].sig_rank = sig_rank;
            }
            return 1;
        }
    if (g_ntruth == MAX_FILES) return 0;      /* full means reject, never accept blind */
    GTruth *t = &g_truth[g_ntruth++];
    snprintf(t->model, sizeof t->model, "%s", model);
    snprintf(t->path, sizeof t->path, "%s", path);
    t->nh = nh;
    t->size = size;
    t->hash = *hash;
    t->has_sig = has_sig;
    t->sig_rank = sig_rank;
    if (has_sig) memcpy(t->sig, sig, 64);
    *hash = NULL;
    return 1;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Reuse history slots after their peer has been stale long enough.  The
 * table is a cap on simultaneous live peers, not on every temporary name a
 * tracker has seen since boot.  refs keeps a relay waiter or an old control
 * connection from losing its mutex/storage underneath it.  Called with
 * g_lk held. */
static Peer *peer_slot_new(int is_expert, int *idx) {
    (void)is_expert; /* both maintainer and expert control links now relay */
    int pick = -1;
    for (int i = 0; i < MAX_PEERS; i++)
        if (!g_peers[i].used) { pick = i; break; }
    if (pick < 0) {
        double now = now_s();
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = &g_peers[i];
            if (now - p->ts <= g_stale_s || p->ctrl_fd >= 0 || p->refs) continue;
            pick = i;
            break;
        }
    }
    if (pick < 0) return NULL;
    Peer *p = &g_peers[pick];
    if (p->used) {
        free(p->files); free(p->ebits); free(p->rq_body); free(p->rq_pay);
        free(p->rq_resp); free(p->rq_resp_pay);
        if (p->evfd >= 0) close(p->evfd);
        pthread_mutex_destroy(&p->rq_lk);
        pthread_cond_destroy(&p->rq_cv);
    }
    memset(p, 0, sizeof *p);
    p->ctrl_fd = -1;
    p->evfd = eventfd(0, EFD_NONBLOCK);
    pthread_mutex_init(&p->rq_lk, NULL);
    pthread_cond_init(&p->rq_cv, NULL);
    g_known_logged[pick] = 0;
    if (idx) *idx = pick;
    return p;
}

static void send_err(int fd, const char *msg) {
    LmbBuf b = {0};
    lmb_buf_str(&b, msg);
    lmb_send(fd, LMB_ERR, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
}

/* ---- REGISTER ----------------------------------------------------------- */

/* Peer identity travels as a fixed 100-byte block at the very end of REGISTER
 * and EREG: {u32 magic, pubkey[32], sig[64]}. Strip it before the rest of the
 * body is parsed, so every existing field parses against the shortened length
 * exactly as before. Returns that shortened length; sets *present and fills
 * pk/sig when the block is there. The signature itself is checked later,
 * once name/model/addr are known, against the connection's nonce. */
static size_t peer_auth_strip(const LmbMsg *m, uint8_t pk[32], uint8_t sig[64],
                              int *present) {
    *present = 0;
    size_t blen = m->body_len;
    if (blen < 100 || lmb_get32(m->body + blen - 100) != LMB_PEER_AUTH_MAGIC)
        return blen;
    const uint8_t *tail = m->body + blen - 100;
    memcpy(pk, tail + 4, 32);
    memcpy(sig, tail + 36, 64);
    *present = 1;
    return blen - 100;
}

/* 0 if this name may be held by this key: unclaimed, or already this key.
 * -1 if the name is owned by a different key (the takeover this closes). */
static int name_key_ok(const char *name, int is_expert, const uint8_t pk[32]) {
    int bi = binding_find(name, is_expert);
    if (bi >= 0 && memcmp(g_bindings[bi].pubkey, pk, 32)) return -1;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert == is_expert &&
            g_peers[i].has_key && !strcmp(g_peers[i].name, name))
            return memcmp(g_peers[i].pubkey, pk, 32) ? -1 : 0;
    return 0;
}

/* Authentication stops anonymous junk from taking a slot, but one attacker
 * can still mint many keys. Cap how many distinct names a single identity may
 * hold, so no key can monopolise the 64-slot table: a real machine uses a
 * handful (serve = origin + executor, a donor one or two). Counts live slots
 * only, so a restart that reclaims the same names is unaffected. */
#define MAX_NAMES_PER_KEY 8
static int identity_has_room(const uint8_t pk[32], const char *name, int is_expert,
                             const char *source) {
    int held = 0, from_source = 0;
    double now = now_s();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!g_peers[i].used || !g_peers[i].has_key) continue;
        if (g_peers[i].is_expert == is_expert && !strcmp(g_peers[i].name, name))
            return 1;                      /* reclaiming a name it already holds */
        int live = now - g_peers[i].ts <= g_stale_s || g_peers[i].ctrl_fd >= 0;
        if (!live) continue;
        if (!memcmp(g_peers[i].pubkey, pk, 32)) held++;
        if (source && *source && !strcmp(g_peers[i].source, source)) from_source++;
    }
    return held < MAX_NAMES_PER_KEY && from_source < g_max_names_per_source;
}

static void connection_source(int fd, char out[64]) {
    out[0] = 0;
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl)) return;
    if (getnameinfo((struct sockaddr *)&ss, sl, out, 64, NULL, 0,
                    NI_NUMERICHOST)) out[0] = 0;
}

/* RSEG callers do not yet carry a signed end-user identity. Bound both work
 * rate and blocked relay threads by observed source address; private swarms
 * additionally retain their existing tracker token. The bucket charges large
 * activation/restore frames more than a control frame. */
static int rseg_rate_enter(int fd, uint32_t pay_len, char source[64]) {
    connection_source(fd, source);
    if (!source[0]) snprintf(source, 64, "unknown");
    double now = now_s();
    double cost = 1.0 + (double)pay_len / (double)(1u << 20);
    pthread_mutex_lock(&g_rseg_rate_lk);
    RsegRate *slot = NULL, *reuse = NULL;
    for (int i = 0; i < RSEG_RATE_SLOTS; i++) {
        RsegRate *candidate = &g_rseg_rates[i];
        if (!strcmp(candidate->source, source)) { slot = candidate; break; }
        if (!candidate->source[0]) reuse = candidate;
        else if (!candidate->in_flight &&
                 (!reuse || candidate->updated < reuse->updated)) reuse = candidate;
    }
    if (!slot) {
        slot = reuse;
        if (slot) {
            memset(slot, 0, sizeof *slot);
            snprintf(slot->source, sizeof slot->source, "%s", source);
            slot->tokens = g_rseg_burst;
            slot->updated = now;
        }
    }
    int allowed = 0;
    if (slot) {
        double elapsed = now - slot->updated;
        if (elapsed > 0) {
            slot->tokens += elapsed * g_rseg_rate_per_s;
            if (slot->tokens > g_rseg_burst) slot->tokens = g_rseg_burst;
            slot->updated = now;
        }
        if (slot->tokens >= cost &&
            slot->in_flight < (uint32_t)g_rseg_source_concurrency) {
            slot->tokens -= cost;
            slot->in_flight++;
            allowed = 1;
        }
    }
    pthread_mutex_unlock(&g_rseg_rate_lk);
    return allowed ? 0 : -1;
}

static void rseg_rate_leave(const char source[64]) {
    pthread_mutex_lock(&g_rseg_rate_lk);
    for (int i = 0; i < RSEG_RATE_SLOTS; i++)
        if (!strcmp(g_rseg_rates[i].source, source)) {
            if (g_rseg_rates[i].in_flight) g_rseg_rates[i].in_flight--;
            break;
        }
    pthread_mutex_unlock(&g_rseg_rate_lk);
}

static Peer *handle_register(int fd, LmbMsg *m, const uint8_t *nonce) {
    uint8_t peer_pk[32], peer_sig[64];
    int have_auth = 0;
    size_t blen = peer_auth_strip(m, peer_pk, peer_sig, &have_auth);
    LmbCur c = { m->body, blen, 0 };
    char name[64], addr[64], model[64];
    char source[64]; connection_source(fd, source);
    uint64_t held, sbytes, sreads;
    uint32_t n;
    if (lmb_cur_str(&c, name, sizeof name) || lmb_cur_str(&c, addr, sizeof addr) ||
        lmb_cur_str(&c, model, sizeof model) || lmb_cur_u64(&c, &held) ||
        lmb_cur_u64(&c, &sbytes) || lmb_cur_u64(&c, &sreads) ||
        lmb_cur_u32(&c, &n) || n > MAX_FILES) { send_err(fd, "bad register"); return NULL; }
    /* Prove the name belongs to this key before anything is bound to it. The
     * signature is over the connection's nonce, so it cannot be replayed onto
     * another connection, and over name+model+addr, so it cannot be lifted
     * onto a different claim. */
    if (!have_auth || !nonce) {
        printf("[tracker] REJECTED: %s did not authenticate its identity\n", name);
        fflush(stdout); send_err(fd, "registration must be signed (CHALLENGE first)");
        return NULL;
    }
    uint8_t authmsg[512];
    size_t aml = lmb_peer_auth_msg(nonce, name, model, addr, authmsg, sizeof authmsg);
    if (!aml || lmb_sign_verify(peer_sig, authmsg, aml, peer_pk)) {
        printf("[tracker] REJECTED: %s has a bad identity signature\n", name);
        fflush(stdout); send_err(fd, "bad identity signature");
        return NULL;
    }
    if (lmb_secure_enabled() && !lmb_secure_peer_matches(fd, peer_pk)) {
        printf("[tracker] REJECTED: %s registration key differs from its "
               "encrypted-channel identity\n", name);
        fflush(stdout); send_err(fd, "channel/registration identity mismatch");
        return NULL;
    }
    PFile *files = (PFile *)calloc(n ? n : 1, sizeof *files);
    if (!files) { send_err(fd, "oom"); return NULL; }
    /* The index is what every chatter builds its local mirror from, so a
     * name that escapes a directory must be stopped HERE as well as at each
     * client: one poisoned announcement would otherwise be handed to
     * everyone who asks, and the oldest clients are the ones least likely to
     * check. */
    for (uint32_t i = 0; i < n; i++) {
        if (lmb_cur_str(&c, files[i].path, sizeof files[i].path) ||
            lmb_cur_u64(&c, &files[i].size)) {
            free(files); send_err(fd, "bad register entry"); return NULL;
        }
        if (!lmb_rel_ok(files[i].path)) {
            printf("[tracker] REJECTED: %s announced an unsafe file name\n", name);
            fflush(stdout);
            free(files); send_err(fd, "unsafe file name"); return NULL;
        }
    }
    /* optional integrity section (older peers simply do not send it) */
    uint8_t **fh = (uint8_t **)calloc(n ? n : 1, sizeof *fh);
    uint32_t *fnh = (uint32_t *)calloc(n ? n : 1, 4);
    uint8_t (*fsig)[64] = (uint8_t (*)[64])calloc(n ? n : 1, 64);
    int *fhas = (int *)calloc(n ? n : 1, sizeof(int));
    size_t save = c.off;
    uint32_t hm = 0;
    int have_h = fh && fnh && fsig && fhas &&
                 !lmb_cur_u32(&c, &hm) && hm == LMB_HASH_MAGIC;
    if (have_h) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t hassig = 0;
            if (lmb_cur_u32(&c, &fnh[i]) || fnh[i] > LMB_MAX_BODY / 32) { have_h = 0; break; }
            if (fnh[i]) {
                fh[i] = (uint8_t *)malloc((size_t)fnh[i] * 32);
                if (!fh[i] || lmb_cur_bytes(&c, fh[i], (size_t)fnh[i] * 32)) { have_h = 0; break; }
            }
            if (lmb_cur_u32(&c, &hassig)) { have_h = 0; break; }
            if (hassig) {
                if (lmb_cur_bytes(&c, fsig[i], 64)) { have_h = 0; break; }
                fhas[i] = 1;
            }
        }
    } else c.off = save;

    LmbModelIdentity mid = {0};
    uint32_t mmagic = 0, mhas = 0;
    int have_mid = 0, bad_mid = 0;
    if (have_h && c.off < c.len) {
        snprintf(mid.model, sizeof mid.model, "%s", model);
        bad_mid = lmb_cur_u32(&c, &mmagic) || mmagic != LMB_MODEL_ID_MAGIC ||
                  lmb_cur_bytes(&c, mid.root, sizeof mid.root) ||
                  lmb_cur_u32(&c, &mhas) || mhas > 1 ||
                  (mhas && lmb_cur_bytes(&c, mid.sig, sizeof mid.sig)) ||
                  c.off != c.len;
        mid.has_sig = mhas != 0;
        have_mid = !bad_mid;
    } else if (have_h && c.off != c.len) bad_mid = 1;

    Peer *slot = NULL;
    int idx = -1, fresh = 0;
    pthread_mutex_lock(&g_lk);
    if (bad_mid || (g_have_pubkey && !have_mid) ||
        (have_mid && !model_identity_check(model, &mid, files, n, fh, fnh))) {
        pthread_mutex_unlock(&g_lk);
        printf("[tracker] REJECTED: %s has a bad or mismatched model identity for %s\n",
               name, model);
        fflush(stdout);
        for (uint32_t i = 0; i < n; i++) free(fh[i]);
        free(fh); free(fnh); free(fsig); free(fhas); free(files);
        send_err(fd, "bad model identity");
        return NULL;
    }
    if (have_h) {
        /* poison dies here: a file whose announced hashes contradict the
         * swarm's ground truth — or, with --pubkey, is not signed by the
         * operator — is stripped from this peer's offer */
        for (uint32_t i = 0; i < n; ) {
            if (!truth_check(model, files[i].path, files[i].size,
                             fnh[i], &fh[i], fsig[i], fhas[i])) {
                printf("[tracker] REJECTED: %s announces %s bytes for %s/%s\n",
                       name, g_have_pubkey && !fhas[i] ? "unsigned" : "different",
                       model, files[i].path);
                fflush(stdout);
                free(fh[i]);
                fh[i] = fh[n - 1]; fnh[i] = fnh[n - 1];
                memcpy(fsig[i], fsig[n - 1], 64); fhas[i] = fhas[n - 1];
                files[i] = files[n - 1];
                n--;
            } else i++;
        }
    } else if (g_have_pubkey && n) {
        printf("[tracker] REJECTED: %s offers %u files with no integrity data\n",
               name, n);
        fflush(stdout);
        n = 0;
    }
    /* trust on first use: the name belongs to the key that first claimed it,
     * and a different key under that name is the takeover this refuses */
    if (name_key_ok(name, 0, peer_pk) < 0) {
        pthread_mutex_unlock(&g_lk);
        printf("[tracker] REJECTED: %s is already held by another key\n", name);
        fflush(stdout);
        for (uint32_t i = 0; i < n; i++) free(fh[i]);
        free(fh); free(fnh); free(fsig); free(fhas); free(files);
        send_err(fd, "name held by another key");
        return NULL;
    }
    if (!identity_has_room(peer_pk, name, 0, source)) {
        pthread_mutex_unlock(&g_lk);
        printf("[tracker] REJECTED: peer identity or source reached its live-name quota\n");
        fflush(stdout);
        for (uint32_t i = 0; i < n; i++) free(fh[i]);
        free(fh); free(fnh); free(fsig); free(fhas); free(files);
        send_err(fd, "peer identity or source admission quota reached");
        return NULL;
    }
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && !g_peers[i].is_expert &&
            !strcmp(g_peers[i].name, name)) { slot = &g_peers[i]; idx = i; break; }
    if (!slot) {
        slot = peer_slot_new(0, &idx);
        fresh = slot != NULL;
    }
    if (slot && binding_check_or_add(name, 0, peer_pk)) {
        pthread_mutex_unlock(&g_lk);
        for (uint32_t i = 0; i < n; i++) free(fh[i]);
        free(fh); free(fnh); free(fsig); free(fhas); free(files);
        send_err(fd, "cannot persist peer identity binding");
        return NULL;
    }
    if (slot) { memcpy(slot->pubkey, peer_pk, 32); slot->has_key = 1; }
    /* observed address: a maintainer that advertises localhost from another
     * machine gets its host part corrected to what this connection shows —
     * the --advertise footgun dies here, ports stay as declared */
    const char *use_addr = addr;
    char fixed[64];
    struct sockaddr_in sin;
    socklen_t sl = sizeof sin;
    if (!strncmp(addr, "127.0.0.1:", 10) &&
        getpeername(fd, (struct sockaddr *)&sin, &sl) == 0 &&
        sin.sin_family == AF_INET &&
        ntohl(sin.sin_addr.s_addr) != INADDR_LOOPBACK) {
        snprintf(fixed, sizeof fixed, "%s:%s", inet_ntoa(sin.sin_addr),
                 strchr(addr, ':') + 1);
        use_addr = fixed;
    }
    if (slot) {
        free(slot->files);
        slot->used = 1; slot->files = files; slot->nfiles = n; slot->ts = now_s();
        slot->held_bytes = held; slot->served_bytes = sbytes; slot->served_reads = sreads;
        slot->ctrl_fd = fd;      /* the newest control connection wins */
        snprintf(slot->name, sizeof slot->name, "%s", name);
        snprintf(slot->addr, sizeof slot->addr, "%s", use_addr);
        snprintf(slot->model, sizeof slot->model, "%s", model);
        snprintf(slot->source, sizeof slot->source, "%s", source);
    }
    pthread_mutex_unlock(&g_lk);
    for (uint32_t i = 0; i < n; i++) free(fh[i]);   /* stolen ones are NULL */
    free(fh); free(fnh); free(fsig); free(fhas);
    if (!slot) { free(files); send_err(fd, "peer table full"); return NULL; }
    if (fresh || !g_known_logged[idx]) {
        printf("[tracker] + %s @ %s (%s, %u files)\n", name, addr, model, n);
        fflush(stdout);
        g_known_logged[idx] = 1;
    }
    lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
    return slot;
}

/* ---- promised expert assignments -----------------------------------------
 * An EASSIGN answer is a promise: the node spends minutes loading its slice
 * before the first EREG heartbeat makes the holding visible. Counting only
 * registered peers made two nodes that asked back-to-back both receive the
 * same "least-covered" slice — half the model assigned twice, the other half
 * to nobody (#52). Remember what was promised and count it as coverage until
 * the EREG arrives (reality supersedes the promise) or it goes stale. All
 * access under g_lk. */
#define MAX_PROMISES 64
#define PROMISE_TTL_S 1800.0
typedef struct {
    int used;
    char model[64], name[64];
    uint8_t *bits; size_t nbytes;        /* same layout as Peer.ebits */
    double ts;
} Promise;
static Promise g_promise[MAX_PROMISES];

static void promise_clear(const char *model, const char *name) {
    for (int i = 0; i < MAX_PROMISES; i++)
        if (g_promise[i].used && !strcmp(g_promise[i].model, model) &&
            !strcmp(g_promise[i].name, name)) {
            free(g_promise[i].bits);
            memset(&g_promise[i], 0, sizeof g_promise[i]);
        }
}

static void promise_store(const char *model, const char *name,
                          const uint8_t *bits, size_t nbytes) {
    Promise *slot = NULL;
    for (int i = 0; i < MAX_PROMISES && !slot; i++)
        if (g_promise[i].used && !strcmp(g_promise[i].model, model) &&
            !strcmp(g_promise[i].name, name)) slot = &g_promise[i];
    for (int i = 0; i < MAX_PROMISES && !slot; i++)
        if (!g_promise[i].used) slot = &g_promise[i];
    if (!slot) {                          /* full: the stalest promise goes */
        slot = &g_promise[0];
        for (int i = 1; i < MAX_PROMISES; i++)
            if (g_promise[i].ts < slot->ts) slot = &g_promise[i];
        free(slot->bits);
        memset(slot, 0, sizeof *slot);
    }
    uint8_t *nb = (uint8_t *)realloc(slot->bits, nbytes);
    if (!nb) return;                      /* a lost promise, not a lost donor */
    memcpy(nb, bits, nbytes);
    slot->bits = nb; slot->nbytes = nbytes;
    slot->used = 1; slot->ts = now_s();
    snprintf(slot->model, sizeof slot->model, "%s", model);
    snprintf(slot->name, sizeof slot->name, "%s", name);
}

/* ---- EREG / EPEERS: expert nodes -----------------------------------------
 * Same shape as REGISTER: a heartbeat that names a model and an address.
 * Expert peers never enter placements (they hold no files); EPEERS is how a
 * chatter learns who can EXECUTE for a model, server included. */

static Peer *handle_ereg(int fd, LmbMsg *m, const uint8_t *nonce) {
    uint8_t peer_pk[32], peer_sig[64];
    int have_auth = 0;
    size_t blen = peer_auth_strip(m, peer_pk, peer_sig, &have_auth);
    LmbCur c = { m->body, blen, 0 };
    char name[64], addr[64], model[64];
    char source[64]; connection_source(fd, source);
    uint32_t nexperts;
    if (lmb_cur_str(&c, name, sizeof name) || lmb_cur_str(&c, addr, sizeof addr) ||
        lmb_cur_str(&c, model, sizeof model) || lmb_cur_u32(&c, &nexperts)) {
        send_err(fd, "bad ereg"); return NULL;
    }
    if (!have_auth || !nonce) {
        printf("[tracker] REJECTED: %s (expert) did not authenticate\n", name);
        fflush(stdout); send_err(fd, "ereg must be signed (CHALLENGE first)");
        return NULL;
    }
    { uint8_t authmsg[512];
      size_t aml = lmb_peer_auth_msg(nonce, name, model, addr, authmsg, sizeof authmsg);
      if (!aml || lmb_sign_verify(peer_sig, authmsg, aml, peer_pk)) {
          printf("[tracker] REJECTED: %s (expert) bad identity signature\n", name);
          fflush(stdout); send_err(fd, "bad identity signature");
          return NULL;
      } }
    if (lmb_secure_enabled() && !lmb_secure_peer_matches(fd, peer_pk)) {
        printf("[tracker] REJECTED: expert %s key differs from its "
               "encrypted-channel identity\n", name);
        fflush(stdout); send_err(fd, "channel/registration identity mismatch");
        return NULL;
    }
    /* optional, appended by newer nodes: the bitmap of what it holds */
    uint32_t bl = 0; const uint8_t *bits = NULL;
    if (!lmb_cur_u32(&c, &bl) && bl && bl <= (1u << 20) && c.off + bl <= c.len) {
        bits = c.p + c.off; c.off += bl;
    } else bl = 0;
    uint32_t meta = 0, qbits = 0, hidden = 0, slots = 0, total_experts = 0;
    uint64_t exec_calls = 0;
    uint32_t exec_inflight = 0, expert_state = 0, resident_flags = 0;
    uint32_t resident_experts = 0;
    uint64_t resident_bytes = 0, resident_vram = 0;
    int have_exec_stats = 0, bad_meta = 0;
    char engine[64] = "", profile[LMB_BUILD_PROFILE_MAX] = "";
    if (c.off < c.len) {
        bad_meta = lmb_cur_u32(&c, &meta) || meta != LMB_EXPERT_MANIFEST_MAGIC ||
                   lmb_cur_str(&c, engine, sizeof engine) ||
                   lmb_cur_str(&c, profile, sizeof profile) ||
                   lmb_cur_u32(&c, &qbits) || lmb_cur_u32(&c, &hidden) ||
                   lmb_cur_u32(&c, &slots) || lmb_cur_u32(&c, &total_experts);
    }
    /* A recognized envelope is optional. Unknown trailing bytes and damaged
     * current-version telemetry are malformed EREG frames. A complete future
     * version may be skipped with its statistics left unavailable. */
    if (!bad_meta && c.off < c.len) {
        uint32_t smagic = 0, sversion = 0, slen = 0;
        if (lmb_cur_u32(&c, &smagic) || smagic != LMB_EREG_STATS_MAGIC ||
            lmb_cur_u32(&c, &sversion) || lmb_cur_u32(&c, &slen) ||
            slen > c.len - c.off) {
            bad_meta = 1;
        } else {
            LmbCur stats = { c.p + c.off, slen, 0 };
            if (sversion == 1u) {
                if (slen != LMB_EREG_STATS_LENGTH_V1 ||
                    lmb_cur_u64(&stats, &exec_calls) ||
                    lmb_cur_u32(&stats, &exec_inflight) || stats.off != stats.len)
                    bad_meta = 1;
                else
                    have_exec_stats = 1;
            } else if (sversion == LMB_EREG_STATS_VERSION) {
                if (slen != LMB_EREG_STATS_LENGTH ||
                    lmb_cur_u64(&stats, &exec_calls) ||
                    lmb_cur_u32(&stats, &exec_inflight) ||
                    lmb_cur_u32(&stats, &expert_state) ||
                    lmb_cur_u32(&stats, &resident_flags) ||
                    lmb_cur_u32(&stats, &resident_experts) ||
                    lmb_cur_u64(&stats, &resident_bytes) ||
                    lmb_cur_u64(&stats, &resident_vram) || stats.off != stats.len)
                    bad_meta = 1;
                else
                    have_exec_stats = 1;
            }
            c.off += slen;
            if (c.off != c.len) bad_meta = 1;
        }
    }
    if (bad_meta) { send_err(fd, "bad ereg metadata"); return NULL; }
    Peer *slot = NULL;
    int fresh = 0;
    pthread_mutex_lock(&g_lk);
    if (name_key_ok(name, 1, peer_pk) < 0) {
        pthread_mutex_unlock(&g_lk);
        printf("[tracker] REJECTED: expert %s is already held by another key\n", name);
        fflush(stdout); send_err(fd, "name held by another key");
        return NULL;
    }
    if (!identity_has_room(peer_pk, name, 1, source)) {
        pthread_mutex_unlock(&g_lk);
        printf("[tracker] REJECTED: peer identity or source reached its live-name quota\n");
        fflush(stdout); send_err(fd, "peer identity or source admission quota reached");
        return NULL;
    }
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert &&
            !strcmp(g_peers[i].name, name)) { slot = &g_peers[i]; break; }
    if (slot && slot->has_segment && strcmp(slot->segment.model, model)) {
        pthread_mutex_unlock(&g_lk);
        send_err(fd, "one compute name cannot mix models");
        return NULL;
    }
    if (!slot) {
        slot = peer_slot_new(1, NULL);
        fresh = slot != NULL;
    }
    if (slot && binding_check_or_add(name, 1, peer_pk)) {
        pthread_mutex_unlock(&g_lk);
        send_err(fd, "cannot persist expert identity binding");
        return NULL;
    }
    if (slot) { memcpy(slot->pubkey, peer_pk, 32); slot->has_key = 1; }
    const char *use_addr = addr;
    char fixed[64];
    struct sockaddr_in sin;
    socklen_t sl = sizeof sin;
    if (!strncmp(addr, "127.0.0.1:", 10) &&
        getpeername(fd, (struct sockaddr *)&sin, &sl) == 0 &&
        sin.sin_family == AF_INET &&
        ntohl(sin.sin_addr.s_addr) != INADDR_LOOPBACK) {
        snprintf(fixed, sizeof fixed, "%s:%s", inet_ntoa(sin.sin_addr),
                 strchr(addr, ':') + 1);
        use_addr = fixed;
    }
    if (slot) {
        slot->used = 1; slot->is_expert = 1; slot->has_expert = 1;
        slot->nexperts = nexperts; slot->ts = now_s();
        slot->expert_ts = slot->ts;
        slot->ctrl_fd = fd;
        snprintf(slot->name, sizeof slot->name, "%s", name);
        snprintf(slot->addr, sizeof slot->addr, "%s", use_addr);
        snprintf(slot->model, sizeof slot->model, "%s", model);
        snprintf(slot->source, sizeof slot->source, "%s", source);
        snprintf(slot->engine, sizeof slot->engine, "%s", engine);
        snprintf(slot->profile, sizeof slot->profile, "%s", profile);
        slot->bits = qbits; slot->hidden = hidden;
        slot->slots = slots; slot->total_experts = total_experts;
        slot->exec_calls = exec_calls;
        slot->exec_inflight = exec_inflight;
        slot->have_exec_stats = have_exec_stats;
        slot->expert_state = expert_state;
        slot->expert_resident_flags = resident_flags;
        slot->resident_experts = resident_experts;
        slot->expert_resident_bytes = resident_bytes;
        slot->expert_vram_bytes = resident_vram;
        if (bl) {
            uint8_t *nb = (uint8_t *)realloc(slot->ebits, bl);
            if (nb) { slot->ebits = nb; memcpy(slot->ebits, bits, bl);
                      slot->enbits = bl * 8; }
            promise_clear(model, name);   /* the promise became a holding */
        }
    }
    pthread_mutex_unlock(&g_lk);
    if (!slot) { send_err(fd, "peer table full"); return NULL; }
    if (fresh) {
        printf("[tracker] + expert %s @ %s (%s, %u experts)\n",
               name, use_addr, model, nexperts);
        fflush(stdout);
    }
    LmbBuf ack = {0};
    lmb_buf_u32(&ack, LMB_EREG_CAP_MAGIC);
    lmb_buf_u32(&ack, LMB_EREG_CAP_VERSION);
    lmb_buf_u32(&ack, LMB_EREG_CAP_STATS);
    if (lmb_send(fd, LMB_OK, ack.p, (uint32_t)ack.len, NULL, 0)) {
        free(ack.p);
        pthread_mutex_lock(&g_lk);
        if (slot->ctrl_fd == fd) slot->ctrl_fd = -1;
        pthread_mutex_unlock(&g_lk);
        return NULL;
    }
    free(ack.p);
    return slot;
}

static int handle_epeers(int fd, LmbMsg *m) {
    char want[64] = "";
    if (m->body_len) {
        LmbCur c = { m->body, m->body_len, 0 };
        if (lmb_cur_str(&c, want, sizeof want)) want[0] = 0;
    }
    LmbBuf b = {0};
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    uint32_t n = 0;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert && g_peers[i].has_expert &&
            now - g_peers[i].expert_ts <= g_stale_s &&
            (!want[0] || !strcmp(g_peers[i].model, want))) n++;
    lmb_buf_u32(&b, n);
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert && g_peers[i].has_expert &&
            now - g_peers[i].expert_ts <= g_stale_s &&
            (!want[0] || !strcmp(g_peers[i].model, want)))
            lmb_buf_str(&b, g_peers[i].addr);
    pthread_mutex_unlock(&g_lk);
    int rc = lmb_send(fd, LMB_EPEERS_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

/* ---- SREG / SEG_ROUTES: stateful range discovery -----------------------
 * A Segment heartbeat shares the compute identity and persistent control
 * connection used by EREG, but it is a distinct capability. This matters for
 * transition releases: a range-only peer must never appear in EPEERS and an
 * old expert-only peer must never be selected for a stateful session. */

static int segment_model_root_ok(const LmbSegAdvert *advert) {
    for (int i = 0; i < MAX_MODELS; i++)
        if (g_models[i].used && !strcmp(g_models[i].id.model, advert->model))
            return !memcmp(g_models[i].id.root, advert->model_root,
                           LMB_SEG_ROOT_BYTES);
    /* Private/development swarms may register compute before the origin. The
     * query still requires an exact root; once a signed origin is known, a
     * conflicting heartbeat is rejected here. */
    return 1;
}

static int segment_route_shape_equal(const LmbSegAdvert *a,
                                     const LmbSegAdvert *b) {
    return !strcmp(a->peer_name, b->peer_name) &&
           !strcmp(a->addr, b->addr) && !strcmp(a->model, b->model) &&
           !memcmp(a->model_root, b->model_root, LMB_SEG_ROOT_BYTES) &&
           !memcmp(a->tokenizer_root, b->tokenizer_root, LMB_SEG_ROOT_BYTES) &&
           a->layer_begin == b->layer_begin && a->layer_end == b->layer_end &&
           a->max_context == b->max_context && a->max_rows == b->max_rows &&
           a->state_dtype == b->state_dtype && a->state_width == b->state_width &&
           a->max_sessions == b->max_sessions && a->flags == b->flags &&
           a->capabilities == b->capabilities &&
           !strcmp(a->engine_id, b->engine_id) &&
           !strcmp(a->state_schema, b->state_schema) &&
           !strcmp(a->numeric_class, b->numeric_class);
}

static Peer *handle_segment_register(int fd, LmbMsg *m, const uint8_t *nonce) {
    if (!g_segment_generation_ready) {
        send_err(fd, "segment discovery is unavailable: no durable generation");
        return NULL;
    }
    uint8_t peer_pk[32], peer_sig[64], next_lease[LMB_SEG_ID_BYTES];
    int have_auth = 0;
    size_t blen = peer_auth_strip(m, peer_pk, peer_sig, &have_auth);
    LmbSegAdvert advert;
    if (lmb_seg_advert_decode(m->body, blen, &advert)) {
        send_err(fd, "bad segment registration"); return NULL;
    }
    if (!have_auth || !nonce) {
        send_err(fd, "segment registration must be signed (CHALLENGE first)");
        return NULL;
    }
    uint8_t authmsg[512];
    size_t auth_len = lmb_peer_auth_msg(nonce, advert.peer_name, advert.model,
                                        advert.addr, authmsg, sizeof authmsg);
    if (!auth_len || lmb_sign_verify(peer_sig, authmsg, auth_len, peer_pk)) {
        send_err(fd, "bad identity signature"); return NULL;
    }
    if (lmb_secure_enabled() && !lmb_secure_peer_matches(fd, peer_pk)) {
        send_err(fd, "channel/registration identity mismatch"); return NULL;
    }
    char source[64]; connection_source(fd, source);
    char fixed[64];
    struct sockaddr_in sin; socklen_t sl = sizeof sin;
    if (!strncmp(advert.addr, "127.0.0.1:", 10) &&
        getpeername(fd, (struct sockaddr *)&sin, &sl) == 0 &&
        sin.sin_family == AF_INET && ntohl(sin.sin_addr.s_addr) != INADDR_LOOPBACK) {
        snprintf(fixed, sizeof fixed, "%s:%s", inet_ntoa(sin.sin_addr),
                 strchr(advert.addr, ':') + 1);
        snprintf(advert.addr, sizeof advert.addr, "%s", fixed);
    }
    lmb_random(next_lease, sizeof next_lease);

    Peer *slot = NULL;
    int fresh = 0, changed = 0;
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    if (!segment_model_root_ok(&advert)) {
        pthread_mutex_unlock(&g_lk);
        send_err(fd, "segment model root differs from tracker identity");
        return NULL;
    }
    if (name_key_ok(advert.peer_name, 1, peer_pk) < 0) {
        pthread_mutex_unlock(&g_lk);
        send_err(fd, "name held by another key"); return NULL;
    }
    if (!identity_has_room(peer_pk, advert.peer_name, 1, source)) {
        pthread_mutex_unlock(&g_lk);
        send_err(fd, "peer identity or source admission quota reached");
        return NULL;
    }
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert &&
            !strcmp(g_peers[i].name, advert.peer_name)) {
            slot = &g_peers[i]; break;
        }
    if (slot && slot->has_expert && strcmp(slot->model, advert.model)) {
        pthread_mutex_unlock(&g_lk);
        send_err(fd, "one compute name cannot mix models");
        return NULL;
    }
    if (!slot) { slot = peer_slot_new(1, NULL); fresh = slot != NULL; }
    if (slot && binding_check_or_add(advert.peer_name, 1, peer_pk)) {
        pthread_mutex_unlock(&g_lk);
        send_err(fd, "cannot persist compute identity binding"); return NULL;
    }
    if (slot) {
        int was_live = slot->has_segment && slot->segment_live &&
                       now - slot->segment_ts <= g_stale_s;
        changed = !was_live || !segment_route_shape_equal(&slot->segment, &advert);
        if (changed) {
            memcpy(slot->segment_owner.lease_id.bytes, next_lease,
                   sizeof slot->segment_owner.lease_id.bytes);
            segment_generation_bump();
            slot->segment_owner.fencing_epoch = g_segment_route_generation;
        }
        slot->segment = advert;
        slot->segment_ts = now; slot->segment_live = 1;
        slot->used = 1; slot->is_expert = 1; slot->has_segment = 1;
        slot->ts = now; slot->ctrl_fd = fd;
        memcpy(slot->pubkey, peer_pk, 32); slot->has_key = 1;
        snprintf(slot->name, sizeof slot->name, "%s", advert.peer_name);
        /* Expert and Segment listeners may use different ports. Keep addr as
         * the expert data-plane endpoint once EREG exists; Segment routes use
         * the independent address stored in slot->segment. */
        if (!slot->has_expert) {
            snprintf(slot->addr, sizeof slot->addr, "%s", advert.addr);
            snprintf(slot->model, sizeof slot->model, "%s", advert.model);
        }
        snprintf(slot->source, sizeof slot->source, "%s", source);
        slot->segment_owner.route_generation = g_segment_route_generation;
        for (int i = 0; i < MAX_SEGMENT_PROMISES; i++)
            if (g_segment_promises[i].used &&
                !strcmp(g_segment_promises[i].name, advert.peer_name))
                g_segment_promises[i].used = 0;
    }
    pthread_mutex_unlock(&g_lk);
    if (!slot) { send_err(fd, "peer table full"); return NULL; }
    if (fresh || changed) {
        printf("[tracker] + segment %s @ %s (%s, layers %u:%u%s)\n",
               advert.peer_name, advert.addr, advert.model,
               advert.layer_begin, advert.layer_end,
               advert.flags & LMB_SEG_ADVERT_DRAINING ? ", draining" : "");
        fflush(stdout);
    }
    uint8_t *reply = NULL; uint32_t reply_len = 0;
    LmbSegOwner owner;
    pthread_mutex_lock(&g_lk);
    owner = slot->segment_owner;
    owner.route_generation = g_segment_route_generation;
    pthread_mutex_unlock(&g_lk);
    if (lmb_seg_registration_reply_encode(&owner, &reply, &reply_len)) {
        send_err(fd, "cannot encode segment lease"); return NULL;
    }
    int rc = lmb_send(fd, LMB_SEG_REGISTER_R, reply, reply_len, NULL, 0);
    free(reply);
    return rc ? NULL : slot;
}

static int handle_segment_routes(int fd, LmbMsg *m) {
    if (!g_segment_generation_ready) {
        send_err(fd, "segment discovery is unavailable: no durable generation");
        return 0;
    }
    LmbSegQuery query;
    if (m->pay_len || lmb_seg_query_decode(m->body, m->body_len, &query)) {
        send_err(fd, "bad segment route query"); return -1;
    }
    LmbSegRouteSnapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    /* Liveness is a material placement change, but only once per transition;
     * repeated queries cannot churn the route generation. */
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (p->used && p->has_segment && p->segment_live &&
            now - p->segment_ts > g_stale_s) {
            p->segment_live = 0;
            segment_generation_bump();
        }
    }
    snapshot.route_generation = g_segment_route_generation;
    for (int i = 0; i < MAX_PEERS && snapshot.count < LMB_SEG_ROUTE_MAX; i++) {
        Peer *p = &g_peers[i];
        if (!p->used || !p->has_segment || !p->segment_live ||
            now - p->segment_ts > g_stale_s ||
            !lmb_seg_advert_compatible(&p->segment, &query)) continue;
        LmbSegRouteEntry *entry = &snapshot.entries[snapshot.count++];
        entry->advert = p->segment;
        entry->owner = p->segment_owner;
        entry->owner.route_generation = snapshot.route_generation;
        if (entry->advert.addr[0] &&
            !(entry->advert.flags & LMB_SEG_ADVERT_RELAY_ONLY))
            entry->transport |= LMB_SEG_TRANSPORT_DIRECT;
        if (p->ctrl_fd >= 0 && p->evfd >= 0)
            entry->transport |= LMB_SEG_TRANSPORT_RELAY;
    }
    snapshot.complete = (uint32_t)lmb_seg_route_complete(&snapshot, &query);
    pthread_mutex_unlock(&g_lk);

    uint8_t *body = NULL; uint32_t body_len = 0;
    if (lmb_seg_route_encode(&snapshot, &body, &body_len)) {
        send_err(fd, "cannot encode segment routes"); return -1;
    }
    int rc = lmb_send(fd, LMB_SEG_ROUTES_R, body, body_len, NULL, 0);
    free(body);
    return rc;
}

/* A donor does not choose layer numbers. It asks for the least-replicated
 * exact fallback slice, then opens that range through the same Colibri ABI
 * and registers normally. Exact origin boundaries guarantee that replacing
 * one donor never makes the full chain unrouteable. */
static int handle_segment_assign(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    uint32_t magic = 0, version = 0;
    char model[LMB_SEG_MODEL_MAX], name[LMB_SEG_PEER_NAME_MAX];
    char engine[LMB_SEG_ENGINE_MAX];
    uint8_t root[LMB_SEG_ROOT_BYTES];
    if (m->pay_len || lmb_cur_u32(&c, &magic) ||
        lmb_cur_u32(&c, &version) || magic != LMB_SEG_ASSIGN_MAGIC ||
        version != LMB_SEG_ASSIGN_VERSION ||
        lmb_cur_str(&c, model, sizeof model) ||
        lmb_cur_str(&c, name, sizeof name) ||
        lmb_cur_str(&c, engine, sizeof engine) ||
        lmb_cur_bytes(&c, root, sizeof root) || c.off != c.len) {
        send_err(fd, "bad segment assignment request"); return -1;
    }
    unsigned nonzero = 0;
    for (size_t i = 0; i < sizeof root; i++) nonzero |= root[i];
    if (!model[0] || !name[0] || !engine[0] || !nonzero) {
        send_err(fd, "incomplete segment assignment identity"); return -1;
    }

    uint32_t chosen_begin = 0, chosen_end = 0, chosen_model_layers = 0;
    uint32_t best_replicas = UINT32_MAX, best_load = UINT32_MAX;
    int retained = 0;
    double now = now_s();
    pthread_mutex_lock(&g_lk);

    /* A restart keeps its previous slice: no unnecessary weight churn. */
    for (int i = 0; i < MAX_PEERS && !chosen_end; i++) {
        Peer *p = &g_peers[i];
        if (p->used && p->has_segment && p->segment_live &&
            now - p->segment_ts <= g_stale_s &&
            !strcmp(p->segment.peer_name, name) &&
            !strcmp(p->segment.model, model) &&
            !strcmp(p->segment.engine_id, engine) &&
            !memcmp(p->segment.model_root, root, sizeof root)) {
            chosen_begin = p->segment.layer_begin;
            chosen_end = p->segment.layer_end;
            retained = 1;
        }
    }
    for (int i = 0; i < MAX_SEGMENT_PROMISES && !chosen_end; i++) {
        SegmentPromise *promise = &g_segment_promises[i];
        if (promise->used && now - promise->ts > SEGMENT_PROMISE_TTL_S)
            promise->used = 0;
        if (promise->used && !strcmp(promise->name, name) &&
            !strcmp(promise->model, model) &&
            !strcmp(promise->engine, engine) &&
            !memcmp(promise->model_root, root, sizeof root)) {
            chosen_begin = promise->begin;
            chosen_end = promise->end;
            retained = 1;
        }
    }

    for (int i = 0; i < MAX_PEERS && !retained; i++) {
        Peer *origin = &g_peers[i];
        if (!origin->used || !origin->has_segment || !origin->segment_live ||
            now - origin->segment_ts > g_stale_s ||
            !(origin->segment.flags & LMB_SEG_ADVERT_FALLBACK) ||
            (origin->segment.flags & LMB_SEG_ADVERT_DRAINING) ||
            strcmp(origin->segment.model, model) ||
            strcmp(origin->segment.engine_id, engine) ||
            memcmp(origin->segment.model_root, root, sizeof root)) continue;
        int duplicate = 0;
        for (int j = 0; j < i; j++) {
            Peer *previous = &g_peers[j];
            if (previous->used && previous->has_segment &&
                previous->segment_live &&
                (previous->segment.flags & LMB_SEG_ADVERT_FALLBACK) &&
                !strcmp(previous->segment.model, model) &&
                !strcmp(previous->segment.engine_id, engine) &&
                !memcmp(previous->segment.model_root, root, sizeof root) &&
                previous->segment.layer_begin == origin->segment.layer_begin &&
                previous->segment.layer_end == origin->segment.layer_end) {
                duplicate = 1; break;
            }
        }
        if (duplicate) continue;

        uint32_t replicas = 0;
        for (int j = 0; j < MAX_PEERS; j++) {
            Peer *peer = &g_peers[j];
            if (!peer->used || !peer->has_segment || !peer->segment_live ||
                now - peer->segment_ts > g_stale_s ||
                (peer->segment.flags & (LMB_SEG_ADVERT_FALLBACK |
                                        LMB_SEG_ADVERT_DRAINING)) ||
                strcmp(peer->segment.model, model) ||
                strcmp(peer->segment.engine_id, engine) ||
                memcmp(peer->segment.model_root, root, sizeof root)) continue;
            if (peer->segment.layer_begin == origin->segment.layer_begin &&
                peer->segment.layer_end == origin->segment.layer_end)
                replicas++;
        }
        for (int j = 0; j < MAX_SEGMENT_PROMISES; j++) {
            SegmentPromise *promise = &g_segment_promises[j];
            if (!promise->used || now - promise->ts > SEGMENT_PROMISE_TTL_S ||
                !strcmp(promise->name, name) ||
                strcmp(promise->model, model) ||
                strcmp(promise->engine, engine) ||
                memcmp(promise->model_root, root, sizeof root)) continue;
            if (promise->begin == origin->segment.layer_begin &&
                promise->end == origin->segment.layer_end)
                replicas++;
        }
        uint64_t raw_load = (uint64_t)origin->segment.active_sessions +
                            origin->segment.queue_depth + origin->segment.inflight;
        uint32_t load = raw_load > UINT32_MAX ? UINT32_MAX : (uint32_t)raw_load;
        if (replicas < best_replicas ||
            (replicas == best_replicas && load < best_load) ||
            (replicas == best_replicas && load == best_load &&
             origin->segment.layer_begin < chosen_begin)) {
            best_replicas = replicas;
            best_load = load;
            chosen_begin = origin->segment.layer_begin;
            chosen_end = origin->segment.layer_end;
        }
    }

    if (chosen_end) {
        SegmentPromise *slot = NULL;
        for (int i = 0; i < MAX_SEGMENT_PROMISES; i++) {
            if (g_segment_promises[i].used &&
                !strcmp(g_segment_promises[i].name, name)) {
                slot = &g_segment_promises[i]; break;
            }
            if (!slot && (!g_segment_promises[i].used ||
                now - g_segment_promises[i].ts > SEGMENT_PROMISE_TTL_S))
                slot = &g_segment_promises[i];
        }
        if (slot) {
            memset(slot, 0, sizeof *slot);
            slot->used = 1; slot->ts = now;
            slot->begin = chosen_begin; slot->end = chosen_end;
            snprintf(slot->model, sizeof slot->model, "%s", model);
            snprintf(slot->name, sizeof slot->name, "%s", name);
            snprintf(slot->engine, sizeof slot->engine, "%s", engine);
            memcpy(slot->model_root, root, sizeof root);
        }
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *origin = &g_peers[i];
        if (origin->used && origin->has_segment && origin->segment_live &&
            now - origin->segment_ts <= g_stale_s &&
            (origin->segment.flags & LMB_SEG_ADVERT_FALLBACK) &&
            !strcmp(origin->segment.model, model) &&
            !strcmp(origin->segment.engine_id, engine) &&
            !memcmp(origin->segment.model_root, root, sizeof root) &&
            origin->segment.layer_end > chosen_model_layers)
            chosen_model_layers = origin->segment.layer_end;
    }
    pthread_mutex_unlock(&g_lk);
    if (!chosen_end || chosen_model_layers < chosen_end) {
        send_err(fd, "no compatible origin Segment slice"); return 0;
    }
    LmbBuf reply = {0};
    lmb_buf_u32(&reply, LMB_SEG_ASSIGN_MAGIC);
    lmb_buf_u32(&reply, LMB_SEG_ASSIGN_VERSION);
    lmb_buf_u32(&reply, chosen_begin);
    lmb_buf_u32(&reply, chosen_end);
    lmb_buf_u32(&reply, chosen_model_layers);
    int rc = lmb_send(fd, LMB_SEG_ASSIGN_R, reply.p,
                      (uint32_t)reply.len, NULL, 0);
    free(reply.p);
    if (retained)
        printf("[tracker] Segment assign %s: %s layers %u:%u (retained)\n",
               name, model, chosen_begin, chosen_end);
    else
        printf("[tracker] Segment assign %s: %s layers %u:%u "
               "(%u replica%s)\n", name, model, chosen_begin, chosen_end,
               best_replicas, best_replicas == 1 ? "" : "s");
    fflush(stdout);
    return rc;
}

static int handle_segment_assign_release(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    uint32_t magic = 0, version = 0;
    char model[LMB_SEG_MODEL_MAX], name[LMB_SEG_PEER_NAME_MAX];
    char engine[LMB_SEG_ENGINE_MAX];
    uint8_t root[LMB_SEG_ROOT_BYTES];
    if (m->pay_len || lmb_cur_u32(&c, &magic) ||
        lmb_cur_u32(&c, &version) || magic != LMB_SEG_ASSIGN_MAGIC ||
        version != LMB_SEG_ASSIGN_VERSION ||
        lmb_cur_str(&c, model, sizeof model) ||
        lmb_cur_str(&c, name, sizeof name) ||
        lmb_cur_str(&c, engine, sizeof engine) ||
        lmb_cur_bytes(&c, root, sizeof root) || c.off != c.len) {
        send_err(fd, "bad segment assignment release"); return -1;
    }
    int released = 0;
    pthread_mutex_lock(&g_lk);
    for (int i = 0; i < MAX_SEGMENT_PROMISES; i++) {
        SegmentPromise *promise = &g_segment_promises[i];
        if (promise->used && !strcmp(promise->model, model) &&
            !strcmp(promise->name, name) &&
            !strcmp(promise->engine, engine) &&
            !memcmp(promise->model_root, root, sizeof root)) {
            memset(promise, 0, sizeof *promise);
            released = 1;
        }
    }
    pthread_mutex_unlock(&g_lk);
    if (released)
        printf("[tracker] Segment assign %s: released before READY\n", name);
    return lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
}

static int handle_ecover(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char model[64], engine[64], profile[LMB_BUILD_PROFILE_MAX];
    uint32_t bits, hidden, slots, nexp;
    if (lmb_cur_str(&c, model, sizeof model) ||
        lmb_cur_str(&c, engine, sizeof engine) ||
        lmb_cur_str(&c, profile, sizeof profile) ||
        lmb_cur_u32(&c, &bits) || lmb_cur_u32(&c, &hidden) ||
        lmb_cur_u32(&c, &slots) || lmb_cur_u32(&c, &nexp) || c.off != c.len ||
        !slots || !nexp || (uint64_t)slots * nexp > (1u << 23)) {
        send_err(fd, "bad ecover"); return -1;
    }
    size_t cells = (size_t)slots * nexp, nb = (cells + 7) / 8;
    uint8_t *cover = (uint8_t *)calloc(nb ? nb : 1, 1);
    if (!cover) { send_err(fd, "oom"); return -1; }
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *q = &g_peers[i];
        /* Relay coverage matches on shape (engine, bits, hidden, slots,
         * experts) but no longer on the full build profile: a relayed EXEC
         * from a peer built with another compiler or checkout is exactly as
         * spot-checkable as a direct one, and demanding byte-identical
         * profiles left half the swarm's coverage invisible. The chatter's
         * own admission gate (lmb_profile_compat, hard fields only) still
         * refuses shape-incompatible peers it talks to directly. */
        if (!q->used || !q->is_expert || !q->has_expert ||
            q->ctrl_fd < 0 || q->evfd < 0 ||
            now - q->expert_ts > g_stale_s || strcmp(q->model, model) ||
            strcmp(q->engine, engine) ||
            q->bits != bits || q->hidden != hidden || q->slots != slots ||
            q->total_experts != nexp) continue;
        size_t take = (q->enbits + 7) / 8;
        if (take > nb) take = nb;
        for (size_t j = 0; j < take; j++) cover[j] |= q->ebits[j];
    }
    pthread_mutex_unlock(&g_lk);
    LmbBuf b = {0};
    lmb_buf_u32(&b, slots); lmb_buf_u32(&b, nexp);
    lmb_buf_u32(&b, (uint32_t)nb); lmb_buf_bytes(&b, cover, nb);
    free(cover);
    int rc = lmb_send(fd, LMB_ECOVER_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

/* ---- HASHES: hand the ground truth to whoever verifies ------------------ */

static int handle_hashes(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char model[64], path[LMB_PATH_MAX];
    if (lmb_cur_str(&c, model, sizeof model) || lmb_cur_str(&c, path, sizeof path)) {
        send_err(fd, "bad hashes request"); return -1;
    }
    pthread_mutex_lock(&g_lk);
    uint32_t nh = 0;
    uint64_t size = 0;
    uint8_t *copy = NULL, sig[64];
    char found_model[64] = "";
    int found = 0, has_sig = 0;
    /* an empty model matches any, exactly as PLACEMENT treats it */
    for (int i = 0; i < g_ntruth; i++)
        if ((!model[0] || !strcmp(g_truth[i].model, model)) &&
            !strcmp(g_truth[i].path, path)) {
            found = 1;
            nh = g_truth[i].nh;
            size = g_truth[i].size;
            has_sig = g_truth[i].has_sig;
            if (has_sig) memcpy(sig, g_truth[i].sig, 64);
            snprintf(found_model, sizeof found_model, "%s", g_truth[i].model);
            if (nh) {
                copy = (uint8_t *)malloc((size_t)nh * 32);
                if (copy) memcpy(copy, g_truth[i].hash, (size_t)nh * 32);
            }
            break;
        }
    pthread_mutex_unlock(&g_lk);
    if (!found) { send_err(fd, "no integrity data"); return 0; }
    if (nh && !copy) { send_err(fd, "oom"); return -1; }
    /* the reply carries everything the verifier needs to rebuild the signed
     * message itself — the tracker is a courier, not a witness to trust */
    LmbBuf b = {0};
    lmb_buf_str(&b, found_model);
    lmb_buf_u32(&b, LMB_HASH_CHUNK);
    lmb_buf_u32(&b, nh);
    lmb_buf_u64(&b, size);
    lmb_buf_u32(&b, has_sig ? 1u : 0u);
    if (has_sig) lmb_buf_bytes(&b, sig, 64);
    int rc = lmb_send(fd, LMB_HASHES_R, b.p, (uint32_t)b.len, copy, nh * 32);
    free(b.p); free(copy);
    return rc;
}

/* ---- MODEL_ID: signed identity of the complete checkpoint --------------- */

static int handle_model_id(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char model[64];
    if (lmb_cur_str(&c, model, sizeof model) || c.off != c.len) {
        send_err(fd, "bad model identity request"); return -1;
    }
    LmbModelIdentity id = {0};
    int found = 0;
    pthread_mutex_lock(&g_lk);
    for (int i = 0; i < MAX_MODELS; i++)
        if (g_models[i].used && !strcmp(g_models[i].id.model, model)) {
            id = g_models[i].id; found = 1; break;
        }
    pthread_mutex_unlock(&g_lk);
    if (!found) { send_err(fd, "no complete model identity"); return 0; }
    LmbBuf b = {0};
    lmb_buf_u32(&b, LMB_MODEL_ID_MAGIC);
    lmb_buf_str(&b, id.model);
    lmb_buf_bytes(&b, id.root, sizeof id.root);
    lmb_buf_u32(&b, id.has_sig ? 1u : 0u);
    if (id.has_sig) lmb_buf_bytes(&b, id.sig, sizeof id.sig);
    int rc = lmb_send(fd, LMB_MODEL_ID_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

/* ---- PLACEMENT ---------------------------------------------------------- */

static int handle_placement(int fd, LmbMsg *m) {
    char want[64] = "";
    if (m->body_len) {
        LmbCur c = { m->body, m->body_len, 0 };
        if (lmb_cur_str(&c, want, sizeof want)) want[0] = 0;
    }
    typedef struct { const PFile *f; char addrs[8][64]; int naddr; } Entry;
    Entry *ent = (Entry *)calloc(MAX_FILES, sizeof *ent);
    int nent = 0;
    if (!ent) { send_err(fd, "oom"); return -1; }
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    for (int p = 0; p < MAX_PEERS; p++) {
        if (!g_peers[p].used || now - g_peers[p].ts > g_stale_s) continue;
        if (want[0] && strcmp(g_peers[p].model, want)) continue;
        for (uint32_t i = 0; i < g_peers[p].nfiles; i++) {
            const PFile *f = &g_peers[p].files[i];
            Entry *e = NULL;
            for (int j = 0; j < nent; j++)
                if (!strcmp(ent[j].f->path, f->path)) { e = &ent[j]; break; }
            if (!e) {
                if (nent == MAX_FILES) continue;
                e = &ent[nent++]; e->f = f;
            }
            if (e->naddr < 8)
                snprintf(e->addrs[e->naddr++], 64, "%s", g_peers[p].addr);
        }
    }
    LmbBuf b = {0};
    lmb_buf_u32(&b, (uint32_t)nent);
    for (int j = 0; j < nent; j++) {
        lmb_buf_str(&b, ent[j].f->path);
        lmb_buf_u64(&b, ent[j].f->size);
        lmb_buf_u16(&b, (uint16_t)ent[j].naddr);
        for (int a = 0; a < ent[j].naddr; a++) lmb_buf_str(&b, ent[j].addrs[a]);
    }
    pthread_mutex_unlock(&g_lk);
    int rc = lmb_send(fd, LMB_PLACEMENT_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p); free(ent);
    return rc;
}

/* ---- SWARM: anonymous by construction ----------------------------------- */

static int handle_swarm(int fd) {
    LmbBuf b = {0}, exec = {0};
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    uint32_t live = 0, live_exec = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!g_peers[i].used || now - g_peers[i].ts > g_stale_s) continue;
        if (g_peers[i].is_expert) {
            if (g_peers[i].has_expert &&
                now - g_peers[i].expert_ts <= g_stale_s) live_exec++;
        } else live++;
    }
    /* Keep this legacy storage-peer prefix byte-for-byte compatible. */
    lmb_buf_u32(&b, live);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (!p->used || p->is_expert || now - p->ts > g_stale_s) continue;
        lmb_buf_str(&b, p->model);
        lmb_buf_u64(&b, p->held_bytes);
        lmb_buf_u64(&b, p->served_bytes);
        lmb_buf_u64(&b, p->served_reads);
        lmb_buf_u32(&b, (uint32_t)(now - p->ts));
        lmb_buf_u32(&b, p->nfiles);
    }
    /* Executors are anonymous and live in a versioned suffix. Old clients
     * stop after the storage rows and ignore these trailing bytes. */
    lmb_buf_u32(&exec, live_exec);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (!p->used || !p->is_expert || !p->has_expert ||
            now - p->expert_ts > g_stale_s) continue;
        lmb_buf_str(&exec, p->model);
        lmb_buf_u32(&exec, p->have_exec_stats ? 1u : 0u);
        lmb_buf_u64(&exec, p->have_exec_stats ? p->exec_calls : 0);
        lmb_buf_u32(&exec, p->have_exec_stats ? p->exec_inflight : 0);
        lmb_buf_u32(&exec, (uint32_t)(now - p->expert_ts));
    }
    pthread_mutex_unlock(&g_lk);
    lmb_buf_u32(&b, LMB_SWARM_EXEC_MAGIC);
    lmb_buf_u32(&b, LMB_SWARM_EXEC_VERSION);
    lmb_buf_u32(&b, (uint32_t)exec.len);
    lmb_buf_bytes(&b, exec.p, exec.len);
    free(exec.p);
    int rc = lmb_send(fd, LMB_SWARM_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

/* Named, versioned status for humans.  The old LMB_SWARM reply stays
 * anonymous and byte-compatible for scripts; this view is intentionally a
 * separate opcode so adding a field can never make an old client misparse
 * the storage prefix.  Addresses and keys are never included. */
static int handle_swarm_detail(int fd) {
    LmbBuf body = {0};
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    uint32_t count = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        int storage = p->used && !p->is_expert && now - p->ts <= g_stale_s;
        int expert = p->used && p->is_expert && p->has_expert &&
                     now - p->expert_ts <= g_stale_s;
        int segment = p->used && p->is_expert && p->has_segment &&
                      p->segment_live && now - p->segment_ts <= g_stale_s;
        if (storage || expert || segment) count++;
    }
    lmb_buf_u32(&body, LMB_SWARM_DETAIL_VERSION);
    lmb_buf_u32(&body, count);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        int storage = p->used && !p->is_expert && now - p->ts <= g_stale_s;
        int expert = p->used && p->is_expert && p->has_expert &&
                     now - p->expert_ts <= g_stale_s;
        int segment = p->used && p->is_expert && p->has_segment &&
                      p->segment_live && now - p->segment_ts <= g_stale_s;
        if (!storage && !expert && !segment) continue;
        uint32_t roles = (storage ? LMB_SWARM_ROLE_STORAGE : 0u) |
                         (expert ? LMB_SWARM_ROLE_EXPERT : 0u) |
                         (segment ? LMB_SWARM_ROLE_SEGMENT : 0u);
        double newest = storage ? p->ts : 0.0;
        if (expert && p->expert_ts > newest) newest = p->expert_ts;
        if (segment && p->segment_ts > newest) newest = p->segment_ts;
        const char *model = segment ? p->segment.model : p->model;
        lmb_buf_str(&body, p->name);
        lmb_buf_str(&body, model);
        lmb_buf_u32(&body, roles);
        lmb_buf_u32(&body, (uint32_t)(now - newest));
        lmb_buf_u64(&body, storage ? p->held_bytes : 0u);
        lmb_buf_u64(&body, storage ? p->served_bytes : 0u);
        lmb_buf_u64(&body, storage ? p->served_reads : 0u);
        lmb_buf_u32(&body, storage ? p->nfiles : 0u);
        lmb_buf_u32(&body, expert ? p->nexperts : 0u);
        lmb_buf_u32(&body, expert && p->have_exec_stats ? 1u : 0u);
        lmb_buf_u64(&body, expert && p->have_exec_stats ? p->exec_calls : 0u);
        lmb_buf_u32(&body, expert && p->have_exec_stats ? p->exec_inflight : 0u);
        lmb_buf_u32(&body, expert ? p->expert_state : 0u);
        lmb_buf_u32(&body, expert ? p->expert_resident_flags : 0u);
        lmb_buf_u32(&body, expert ? p->resident_experts : 0u);
        lmb_buf_u64(&body, expert ? p->expert_resident_bytes : 0u);
        lmb_buf_u64(&body, expert ? p->expert_vram_bytes : 0u);
        lmb_buf_u32(&body, segment ? p->segment.layer_begin : 0u);
        lmb_buf_u32(&body, segment ? p->segment.layer_end : 0u);
        lmb_buf_u32(&body, segment ? p->segment.active_sessions : 0u);
        lmb_buf_u32(&body, segment ? p->segment.max_sessions : 0u);
        lmb_buf_u32(&body, segment ? p->segment.queue_depth : 0u);
        lmb_buf_u32(&body, segment ? p->segment.inflight : 0u);
        lmb_buf_u32(&body, segment ? p->segment.flags : 0u);
    }
    pthread_mutex_unlock(&g_lk);
    int rc = lmb_send(fd, LMB_SWARM_DETAIL_R, body.p,
                      (uint32_t)body.len, NULL, 0);
    free(body.p);
    return rc;
}

/* ---- EASSIGN: which experts should this node hold? ----------------------
 *
 * The disk side has had this since day one: a donor offers bytes, the tracker
 * answers with the least-replicated files. The compute side had nothing, so a
 * donor had to be told `--stride 9:3` — which means knowing how many other
 * donors exist and which index is free. That is coordination, and coordination
 * is what a swarm is supposed to remove.
 *
 * Now a node says only what it knows about ITSELF: how many experts it can
 * hold, the shape of the model, which slots route, and what it already has.
 * The tracker answers with a set, rarest first.
 *
 * Two rules keep it stable. What the node already holds is kept (a restart
 * must not re-download), and the count for each expert excludes the asker, so
 * "rare" means rare among the OTHERS — otherwise a node would drop exactly
 * what it is the only holder of. */
static int handle_eassign(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char model[64], name[64];
    uint32_t slots, nexp, capacity, rlen;
    if (lmb_cur_str(&c, model, sizeof model) || lmb_cur_str(&c, name, sizeof name) ||
        lmb_cur_u32(&c, &slots) || lmb_cur_u32(&c, &nexp) ||
        lmb_cur_u32(&c, &capacity) || lmb_cur_u32(&c, &rlen) || !slots || !nexp ||
        (uint64_t)slots * nexp > (1u << 24) || rlen != slots ||
        c.off + rlen > c.len) {
        send_err(fd, "bad eassign"); return -1;
    }
    const uint8_t *routed = c.p + c.off; c.off += rlen;

    size_t cells = (size_t)slots * nexp;
    uint16_t *cnt = (uint16_t *)calloc(cells, sizeof *cnt);
    uint32_t *pick = (uint32_t *)malloc(cells * sizeof *pick);
    uint8_t *mine = (uint8_t *)calloc((cells + 7) / 8, 1);
    if (!cnt || !pick || !mine) {
        free(cnt); free(pick); free(mine); send_err(fd, "oom"); return -1; }

    /* Replica counts over every OTHER live executor, and separately what THIS
     * one held before. Both matter and for opposite reasons: counting itself
     * would make a node drop precisely what it is the only holder of, and
     * forgetting what it held would make every restart re-download a
     * different slice. The tracker is the memory here — the node sends
     * nothing about its past, because on a fresh start it has no past and
     * would have to claim it holds everything. */
    /* Deterministic replica rank, for the elastic release below: among the
     * holders of one expert, order by a hash of the NAME mixed with the cell, so the surplus spreads across nodes instead of electing one loser. Only the tracker
     * computes it, so two nodes can never disagree about who is surplus —
     * the failure mode where every replica politely releases the same cell
     * at the same time and coverage drops to zero. */
    uint64_t myh = 1469598103934665603ull;
    for (const char *s = name; *s; s++) { myh ^= (uint8_t)*s; myh *= 1099511628211ull; }
    uint8_t *lower = (uint8_t *)calloc(cells, 1);   /* higher-rank holders */
    if (!lower) { free(cnt); free(pick); free(mine); send_err(fd, "oom"); return -1; }

    pthread_mutex_lock(&g_lk);
    double now = now_s();
    int have_mine = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (!p->used || !p->is_expert || !p->has_expert ||
            strcmp(p->model, model) || !p->ebits) continue;
        if (!strcmp(p->name, name)) {                 /* this node, as it was */
            memcpy(mine, p->ebits, ((cells + 7) / 8 < (p->enbits + 7) / 8
                                    ? (cells + 7) / 8 : (p->enbits + 7) / 8));
            have_mine = 1;
            continue;
        }
        if (now - p->expert_ts > g_stale_s) continue;
        uint64_t ph = 1469598103934665603ull;
        for (const char *s = p->name; *s; s++) { ph ^= (uint8_t)*s; ph *= 1099511628211ull; }
        for (size_t k = 0; k < cells && k < p->enbits; k++)
            if (p->ebits[k >> 3] & (1u << (k & 7))) {
                cnt[k]++;
                uint64_t mix = (uint64_t)k * 0x9E3779B97F4A7C15ull;
                if ((ph ^ mix) < (myh ^ mix) && lower[k] < 255) lower[k]++;
            }
    }
    /* Slices promised to nodes still loading count as coverage too (#52),
     * and this node's own promise is its past on a re-ask mid-load. A live
     * EREG under the same name has already been counted above and wins. */
    for (int i = 0; i < MAX_PROMISES; i++) {
        Promise *pr = &g_promise[i];
        if (!pr->used || strcmp(pr->model, model)) continue;
        if (now - pr->ts > PROMISE_TTL_S) continue;
        if (!strcmp(pr->name, name)) {
            if (!have_mine) {
                size_t nb = (cells + 7) / 8 < pr->nbytes ? (cells + 7) / 8
                                                         : pr->nbytes;
                memcpy(mine, pr->bits, nb);
            }
            continue;
        }
        int live = 0;
        for (int j = 0; j < MAX_PEERS; j++)
            if (g_peers[j].used && g_peers[j].is_expert &&
                g_peers[j].has_expert && g_peers[j].ebits &&
                now - g_peers[j].expert_ts <= g_stale_s &&
                !strcmp(g_peers[j].name, pr->name) &&
                !strcmp(g_peers[j].model, model)) { live = 1; break; }
        if (live) continue;
        uint64_t ph = 1469598103934665603ull;
        for (const char *s = pr->name; *s; s++) { ph ^= (uint8_t)*s; ph *= 1099511628211ull; }
        for (size_t k = 0; k < cells && (k >> 3) < pr->nbytes; k++)
            if (pr->bits[k >> 3] & (1u << (k & 7))) {
                cnt[k]++;
                uint64_t mix = (uint64_t)k * 0x9E3779B97F4A7C15ull;
                if ((ph ^ mix) < (myh ^ mix) && lower[k] < 255) lower[k]++;
            }
    }
    pthread_mutex_unlock(&g_lk);

    /* pass 1: keep what it already had — no churn, no re-download — UNLESS
     * the expert is a surplus copy: this node ranks at or below the keep
     * limit among its holders. Releasing it is what makes the share elastic:
     * as donors join, over-replicated experts are let go and the freed
     * capacity refills with what the swarm still lacks. Released cells are
     * marked unpickable for THIS reply, or pass 2 would hand them straight
     * back and nothing would ever shrink. */
    #define EASSIGN_KEEP_REPLICAS 2
    uint32_t n = 0;
    for (size_t k = 0; k < cells && n < capacity; k++) {
        if (!routed[k / nexp]) continue;
        if (mine[k >> 3] & (1u << (k & 7))) {
            if (lower[k] >= EASSIGN_KEEP_REPLICAS) {
                cnt[k] = 0xfffe;                 /* released: not pickable now */
                continue;
            }
            pick[n++] = (uint32_t)k;
            cnt[k] = 0xffff;                     /* taken: never picked twice */
        }
    }
    /* pass 2: whole layers first, least-covered layer first. Phase 2 gates
     * per layer — a layer fully held by donors runs on the swarm while its
     * neighbours run locally — so completing layers is worth strictly more
     * than scattering the same count across all of them: every completed
     * layer removes one WAN round from every token that routes through it.
     * A scattered 5% used to light up exactly nothing. */
    while (n < capacity) {
        int best = -1;
        long best_cov = 0;
        uint32_t best_free = 0;
        for (uint32_t l = 0; l < slots; l++) {
            if (!routed[l]) continue;
            long cov = 0;
            uint32_t fc = 0;
            for (uint32_t e = 0; e < nexp; e++) {
                size_t k = (size_t)l * nexp + e;
                if (cnt[k] >= 0xfffe) continue;      /* mine, picked or released */
                cov += cnt[k];
                fc++;
            }
            if (!fc || fc > capacity - n) continue;  /* complete, or won't fit */
            if (best < 0 || cov < best_cov) { best = (int)l; best_cov = cov; best_free = fc; }
        }
        if (best < 0) break;
        (void)best_free;
        for (uint32_t e = 0; e < nexp && n < capacity; e++) {
            size_t k = (size_t)best * nexp + e;
            if (cnt[k] >= 0xfffe) continue;
            pick[n++] = (uint32_t)k;
            cnt[k] = 0xffff;
        }
    }
    /* pass 3: capacity smaller than any remaining layer — the old
     * rarest-first scattering fills what is left */
    for (uint16_t want = 0; want < 64 && n < capacity; want++)
        for (size_t k = 0; k < cells && n < capacity; k++) {
            if (!routed[k / nexp] || cnt[k] != want) continue;
            pick[n++] = (uint32_t)k;
            cnt[k] = 0xffff;
        }

    /* remember the promise, so the NEXT asker sees this slice as covered */
    {
        size_t nb = (cells + 7) / 8;
        uint8_t *pb = (uint8_t *)calloc(nb, 1);
        if (pb) {
            for (uint32_t i = 0; i < n; i++)
                pb[pick[i] >> 3] |= (uint8_t)(1u << (pick[i] & 7));
            pthread_mutex_lock(&g_lk);
            promise_store(model, name, pb, nb);
            pthread_mutex_unlock(&g_lk);
            free(pb);
        }
    }

    LmbBuf b = {0};
    lmb_buf_u32(&b, n);
    for (uint32_t i = 0; i < n; i++) {
        lmb_buf_u32(&b, pick[i] / nexp);
        lmb_buf_u32(&b, pick[i] % nexp);
    }
    free(cnt); free(pick); free(mine); free(lower);
    int rc = lmb_send(fd, LMB_EASSIGN_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    printf("[tracker] assign: %u experts of %s to an executor "
           "(capacity %u)\n", n, model, capacity);
    fflush(stdout);
    return rc;
}

/* ---- RREAD: the relay --------------------------------------------------- */

static int handle_rread(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char model[64], path[LMB_PATH_MAX];
    uint64_t off; uint32_t len;
    if (lmb_cur_str(&c, model, sizeof model) || lmb_cur_str(&c, path, sizeof path) ||
        lmb_cur_u64(&c, &off) || lmb_cur_u32(&c, &len) || len > LMB_MAX_PAY) {
        send_err(fd, "bad rread"); return -1;
    }
    /* pick a live, relay-capable peer that holds the file */
    Peer *p = NULL;
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    for (int i = 0; i < MAX_PEERS && !p; i++) {
        Peer *q = &g_peers[i];
        if (!q->used || now - q->ts > g_stale_s || q->ctrl_fd < 0 || q->evfd < 0) continue;
        if (model[0] && strcmp(q->model, model)) continue;
        for (uint32_t j = 0; j < q->nfiles; j++)
            if (!strcmp(q->files[j].path, path)) { p = q; p->refs++; break; }
    }
    pthread_mutex_unlock(&g_lk);
    if (!p) { send_err(fd, "no relay-capable peer holds it"); return 0; }

    pthread_mutex_lock(&p->rq_lk);
    while (p->rq_busy) pthread_cond_wait(&p->rq_cv, &p->rq_lk);
    p->rq_busy = 1; p->rq_sent = 0; p->rq_done = 0; p->rq_ok = 0;
    p->rq_id = ++p->rq_next; p->rq_op = LMB_RREAD_FWD;
    snprintf(p->rq_path, sizeof p->rq_path, "%s", path);
    p->rq_off = off; p->rq_len = len;
    free(p->rq_body); p->rq_body = NULL; p->rq_body_len = 0;
    free(p->rq_pay); p->rq_pay = NULL; p->rq_pay_len = 0;
    free(p->rq_resp); p->rq_resp = NULL; p->rq_resp_len = 0;
    free(p->rq_resp_pay); p->rq_resp_pay = NULL; p->rq_resp_pay_len = 0;
    pthread_mutex_unlock(&p->rq_lk);

    uint64_t one = 1;
    if (write(p->evfd, &one, 8) != 8) { /* control thread also polls the socket */ }

    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += RELAY_WAIT_S;
    pthread_mutex_lock(&p->rq_lk);
    while (!p->rq_done)
        if (pthread_cond_timedwait(&p->rq_cv, &p->rq_lk, &dl) != 0) break;
    int ok = p->rq_done && p->rq_ok;
    uint8_t *resp = p->rq_resp; uint32_t resp_len = p->rq_resp_len;
    p->rq_resp = NULL; p->rq_resp_len = 0;
    p->rq_busy = 0;
    pthread_cond_broadcast(&p->rq_cv);
    pthread_mutex_unlock(&p->rq_lk);

    int rc;
    if (!ok) { send_err(fd, "relay timeout or peer failure"); rc = 0; }
    else {
        LmbBuf b = {0};
        lmb_buf_u32(&b, 1);
        rc = lmb_send(fd, LMB_RREAD_R, b.p, (uint32_t)b.len, resp, resp_len);
        free(b.p);
    }
    free(resp);
    pthread_mutex_lock(&g_lk);
    if (p->refs) p->refs--;
    pthread_mutex_unlock(&g_lk);
    return rc;
}

/* EXEC relay: same one-request mailbox as byte relay, but the expert node's
 * EREG connection is the outbound tunnel.  The tracker validates enough of
 * the shape to select a real holder and bound memory; the expert validates
 * the complete engine-specific request before executing it. */
static int handle_rexec(int fd, LmbMsg *m) {
    if (getenv("LUMABRI_RELAY_TRACE"))
        fprintf(stderr, "[tracker] rexec request body=%u pay=%u\n",
                m->body_len, m->pay_len);
    LmbCur c = { m->body, m->body_len, 0 };
    char model[64];
    uint32_t nexp = 0, layer = 0, eid = 0, dim = 0, nrows = 0;
    if (lmb_cur_str(&c, model, sizeof model) || lmb_cur_u32(&c, &nexp) ||
        lmb_cur_u32(&c, &layer) || lmb_cur_u32(&c, &eid) ||
        lmb_cur_u32(&c, &dim) || lmb_cur_u32(&c, &nrows)) {
        send_err(fd, "malformed rexec header"); return -1;
    }
    if (nexp < 1 || eid >= nexp ||
        !dim || nrows < 1 || nrows > LMB_MAX_EXEC_ROWS ||
        (m->body_len - c.off != 0 && m->body_len - c.off != (uint64_t)nrows * 4) ||
        (uint64_t)nrows * dim * 4 != m->pay_len) {
        char why[160];
        snprintf(why, sizeof why, "bad rexec shape (nexp=%u layer=%u eid=%u "
                 "dim=%u rows=%u body_tail=%zu pay=%u)", nexp, layer, eid,
                 dim, nrows, m->body_len - c.off, m->pay_len);
        send_err(fd, why); return -1;
    }
    size_t exec_at = c.off - 16;             /* layer starts 16 bytes earlier */
    uint64_t gid64 = (uint64_t)layer * nexp + eid;
    if (gid64 > UINT32_MAX) { send_err(fd, "bad rexec expert id"); return -1; }

    Peer *p = NULL;
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    for (int i = 0; i < MAX_PEERS && !p; i++) {
        Peer *q = &g_peers[i];
        uint32_t gid = (uint32_t)gid64;
        if (!q->used || !q->is_expert || !q->has_expert ||
            now - q->expert_ts > g_stale_s ||
            q->ctrl_fd < 0 || q->evfd < 0 || strcmp(q->model, model) || gid >= q->enbits ||
            !(q->ebits[gid >> 3] & (uint8_t)(1u << (gid & 7)))) continue;
        p = q; p->refs++;
    }
    pthread_mutex_unlock(&g_lk);
    if (!p) { send_err(fd, "no relay-capable expert holds it"); return 0; }
    if (getenv("LUMABRI_RELAY_TRACE"))
        fprintf(stderr, "[tracker] rexec selected %s ctrl=%d gid=%llu\n",
                p->name, p->ctrl_fd, (unsigned long long)gid64);

    uint32_t body_len = m->body_len - (uint32_t)exec_at;
    uint8_t *body = (uint8_t *)malloc(body_len ? body_len : 1);
    uint8_t *pay = (uint8_t *)malloc(m->pay_len ? m->pay_len : 1);
    if (!body || !pay) {
        free(body); free(pay);
        pthread_mutex_lock(&g_lk); if (p->refs) p->refs--; pthread_mutex_unlock(&g_lk);
        send_err(fd, "oom"); return -1;
    }
    memcpy(body, m->body + exec_at, body_len);
    memcpy(pay, m->pay, m->pay_len);

    pthread_mutex_lock(&p->rq_lk);
    while (p->rq_busy) pthread_cond_wait(&p->rq_cv, &p->rq_lk);
    p->rq_busy = 1; p->rq_sent = 0; p->rq_done = 0; p->rq_ok = 0;
    p->rq_id = ++p->rq_next; p->rq_op = LMB_REXEC_FWD;
    free(p->rq_body); p->rq_body = body; p->rq_body_len = body_len;
    free(p->rq_pay); p->rq_pay = pay; p->rq_pay_len = m->pay_len;
    free(p->rq_resp); p->rq_resp = NULL; p->rq_resp_len = 0;
    free(p->rq_resp_pay); p->rq_resp_pay = NULL; p->rq_resp_pay_len = 0;
    pthread_mutex_unlock(&p->rq_lk);
    uint64_t one = 1; if (write(p->evfd, &one, 8) != 8) {}
    if (getenv("LUMABRI_RELAY_TRACE"))
        fprintf(stderr, "[tracker] rexec queued id=%u evfd=%d\n", p->rq_id, p->evfd);

    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += RELAY_WAIT_S;
    pthread_mutex_lock(&p->rq_lk);
    while (!p->rq_done)
        if (pthread_cond_timedwait(&p->rq_cv, &p->rq_lk, &dl) != 0) break;
    int ok = p->rq_done && p->rq_ok;
    uint8_t *resp = p->rq_resp; uint32_t resp_len = p->rq_resp_len;
    p->rq_resp = NULL; p->rq_resp_len = 0;
    free(p->rq_body); p->rq_body = NULL; p->rq_body_len = 0;
    free(p->rq_pay); p->rq_pay = NULL; p->rq_pay_len = 0;
    p->rq_busy = 0; pthread_cond_broadcast(&p->rq_cv);
    pthread_mutex_unlock(&p->rq_lk);

    int rc;
    if (!ok) { free(resp); send_err(fd, "exec relay timeout or peer failure"); rc = 0; }
    else {
        LmbBuf b = {0}; lmb_buf_u32(&b, 1);
        rc = lmb_send(fd, LMB_REXEC_R, b.p, (uint32_t)b.len, resp, resp_len);
        free(b.p); free(resp);
    }
    pthread_mutex_lock(&g_lk); if (p->refs) p->refs--; pthread_mutex_unlock(&g_lk);
    return rc;
}

static int segment_request_op(uint32_t op) {
    return op == LMB_SEG_OPEN || op == LMB_SEG_RUN ||
           op == LMB_SEG_SNAPSHOT || op == LMB_SEG_RESTORE ||
           op == LMB_SEG_CLOSE || op == LMB_SEG_HEALTH;
}

/* Relay one ordinary Segment frame to one exact signed Segment registrant.
 * Selection is by peer identity, never "any peer with this range": stateful
 * placement belongs to the session and failover is an explicit client act. */
static int handle_rseg(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char peer_name[LMB_SEG_PEER_NAME_MAX];
    uint32_t inner_op = 0, inner_body_len = 0;
    if (lmb_cur_str(&c, peer_name, sizeof peer_name) ||
        lmb_cur_u32(&c, &inner_op) ||
        lmb_cur_u32(&c, &inner_body_len) ||
        !segment_request_op(inner_op) ||
        inner_body_len != c.len - c.off ||
        !lmb_frame_shape_ok(inner_op, inner_body_len, m->pay_len)) {
        send_err(fd, "bad Segment relay frame"); return -1;
    }
    char rate_source[64];
    if (rseg_rate_enter(fd, m->pay_len, rate_source)) {
        send_err(fd, "Segment relay source rate/concurrency limit");
        return 0;
    }

    Peer *p = NULL;
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *q = &g_peers[i];
        if (!q->used || !q->is_expert || !q->has_segment ||
            !q->segment_live || now - q->segment_ts > g_stale_s ||
            q->ctrl_fd < 0 || q->evfd < 0 ||
            strcmp(q->segment.peer_name, peer_name)) continue;
        p = q; p->refs++; break;
    }
    pthread_mutex_unlock(&g_lk);
    if (!p) {
        rseg_rate_leave(rate_source);
        send_err(fd, "Segment relay peer is unavailable"); return 0;
    }

    uint8_t *body = malloc(inner_body_len ? inner_body_len : 1u);
    uint8_t *pay = malloc(m->pay_len ? m->pay_len : 1u);
    if (!body || !pay) {
        free(body); free(pay);
        pthread_mutex_lock(&g_lk); if (p->refs) p->refs--; pthread_mutex_unlock(&g_lk);
        rseg_rate_leave(rate_source);
        send_err(fd, "oom"); return -1;
    }
    if (inner_body_len) memcpy(body, c.p + c.off, inner_body_len);
    if (m->pay_len) memcpy(pay, m->pay, m->pay_len);

    pthread_mutex_lock(&p->rq_lk);
    struct timespec queue_deadline;
    clock_gettime(CLOCK_REALTIME, &queue_deadline);
    queue_deadline.tv_sec += g_rseg_queue_ms / 1000;
    queue_deadline.tv_nsec += (long)(g_rseg_queue_ms % 1000) * 1000000L;
    if (queue_deadline.tv_nsec >= 1000000000L) {
        queue_deadline.tv_sec++;
        queue_deadline.tv_nsec -= 1000000000L;
    }
    while (p->rq_busy)
        if (pthread_cond_timedwait(&p->rq_cv, &p->rq_lk,
                                   &queue_deadline) == ETIMEDOUT) break;
    if (p->rq_busy) {
        pthread_mutex_unlock(&p->rq_lk);
        free(body); free(pay);
        pthread_mutex_lock(&g_lk); if (p->refs) p->refs--; pthread_mutex_unlock(&g_lk);
        rseg_rate_leave(rate_source);
        send_err(fd, "Segment relay executor queue is busy");
        return 0;
    }
    p->rq_busy = 1; p->rq_sent = 0; p->rq_done = 0; p->rq_ok = 0;
    p->rq_id = ++p->rq_next; p->rq_op = LMB_RSEG_FWD;
    p->rq_inner_op = inner_op;
    free(p->rq_body); p->rq_body = body; p->rq_body_len = inner_body_len;
    free(p->rq_pay); p->rq_pay = pay; p->rq_pay_len = m->pay_len;
    free(p->rq_resp); p->rq_resp = NULL; p->rq_resp_len = 0;
    free(p->rq_resp_pay); p->rq_resp_pay = NULL; p->rq_resp_pay_len = 0;
    p->rq_resp_op = 0;
    pthread_mutex_unlock(&p->rq_lk);
    uint64_t one = 1; if (write(p->evfd, &one, sizeof one) != sizeof one) {}

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += RELAY_WAIT_S;
    pthread_mutex_lock(&p->rq_lk);
    while (!p->rq_done)
        if (pthread_cond_timedwait(&p->rq_cv, &p->rq_lk, &deadline)) break;
    int ok = p->rq_done && p->rq_ok;
    uint32_t response_op = p->rq_resp_op;
    uint8_t *response_body = p->rq_resp;
    uint32_t response_body_len = p->rq_resp_len;
    uint8_t *response_pay = p->rq_resp_pay;
    uint32_t response_pay_len = p->rq_resp_pay_len;
    p->rq_resp = NULL; p->rq_resp_len = 0;
    p->rq_resp_pay = NULL; p->rq_resp_pay_len = 0;
    free(p->rq_body); p->rq_body = NULL; p->rq_body_len = 0;
    free(p->rq_pay); p->rq_pay = NULL; p->rq_pay_len = 0;
    p->rq_busy = 0; pthread_cond_broadcast(&p->rq_cv);
    pthread_mutex_unlock(&p->rq_lk);

    int rc = 0;
    if (!ok) {
        send_err(fd, "Segment relay timeout or peer failure");
    } else {
        LmbBuf reply = {0};
        if (lmb_buf_u32(&reply, response_op) ||
            lmb_buf_u32(&reply, response_body_len) ||
            lmb_buf_bytes(&reply, response_body, response_body_len))
            rc = -1;
        else
            rc = lmb_send(fd, LMB_RSEG_R, reply.p, (uint32_t)reply.len,
                          response_pay, response_pay_len);
        free(reply.p);
    }
    free(response_body); free(response_pay);
    pthread_mutex_lock(&g_lk); if (p->refs) p->refs--; pthread_mutex_unlock(&g_lk);
    rseg_rate_leave(rate_source);
    return rc;
}

/* ---- ASSIGN: the server decides --------------------------------------------
 * A donor offers a byte budget and lists what it already holds; the answer
 * is the slice it should pull and serve. Policy: RAREST FIRST — the files
 * of the model with the fewest live replicas come first, so every new donor
 * thickens the swarm exactly where it is thinnest. (MeshLLM hands out static
 * layer ranges; replication driven by measured scarcity heals churn too.) */
static int handle_assign(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char model[64];
    uint64_t budget;
    uint32_t nhave;
    if (lmb_cur_str(&c, model, sizeof model) || lmb_cur_u64(&c, &budget) ||
        lmb_cur_u32(&c, &nhave) || nhave > MAX_FILES) {
        send_err(fd, "bad assign"); return -1;
    }
    char (*have)[LMB_PATH_MAX] = calloc(nhave ? nhave : 1, LMB_PATH_MAX);
    if (!have) { send_err(fd, "oom"); return -1; }
    for (uint32_t i = 0; i < nhave; i++)
        if (lmb_cur_str(&c, have[i], LMB_PATH_MAX)) { free(have); send_err(fd, "bad assign"); return -1; }

    typedef struct { char path[LMB_PATH_MAX]; uint64_t size; int replicas; } Cand;
    Cand *cand = calloc(MAX_FILES, sizeof *cand);
    int ncand = 0;
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    for (int p = 0; p < MAX_PEERS; p++) {
        Peer *q = &g_peers[p];
        if (!q->used || now - q->ts > g_stale_s) continue;
        if (model[0] && strcmp(q->model, model)) continue;
        for (uint32_t i = 0; i < q->nfiles; i++) {
            Cand *e = NULL;
            for (int j = 0; j < ncand; j++)
                if (!strcmp(cand[j].path, q->files[i].path)) { e = &cand[j]; break; }
            if (!e) {
                if (ncand == MAX_FILES) continue;
                e = &cand[ncand++];
                snprintf(e->path, sizeof e->path, "%s", q->files[i].path);
                e->size = q->files[i].size;
            }
            e->replicas++;
        }
    }
    pthread_mutex_unlock(&g_lk);

    /* drop what the donor already has */
    for (int j = 0; j < ncand; ) {
        int skip = 0;
        for (uint32_t i = 0; i < nhave; i++)
            if (!strcmp(have[i], cand[j].path)) { skip = 1; break; }
        if (skip) cand[j] = cand[--ncand]; else j++;
    }
    /* rarest first; among equals, biggest first (move real bytes) */
    for (int a = 0; a < ncand; a++)
        for (int b = a + 1; b < ncand; b++)
            if (cand[b].replicas < cand[a].replicas ||
                (cand[b].replicas == cand[a].replicas && cand[b].size > cand[a].size)) {
                Cand t = cand[a]; cand[a] = cand[b]; cand[b] = t;
            }
    LmbBuf out = {0};
    uint32_t picked = 0;
    uint64_t left = budget;
    size_t count_at = 0;
    lmb_buf_u32(&out, 0);          /* patched below */
    count_at = 0;
    for (int j = 0; j < ncand; j++)
        if (cand[j].size <= left) {
            lmb_buf_str(&out, cand[j].path);
            lmb_buf_u64(&out, cand[j].size);
            left -= cand[j].size;
            picked++;
        }
    lmb_put32(out.p + count_at, picked);
    int rc = lmb_send(fd, LMB_ASSIGN_R, out.p, (uint32_t)out.len, NULL, 0);
    printf("[tracker] assign: %u files (%.1f GB) of %s to a donor\n",
           picked, (double)(budget - left) / 1e9, model[0] ? model : "any");
    fflush(stdout);
    free(out.p); free(cand); free(have);
    return rc;
}

/* the peer's control thread completes the pending request */
static void relay_complete(Peer *p, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    uint32_t id = 0, ok = 0;
    if (lmb_cur_u32(&c, &id) || lmb_cur_u32(&c, &ok)) return;
    if (getenv("LUMABRI_RELAY_TRACE"))
        fprintf(stderr, "[tracker] relay complete op=%u id=%u ok=%u pay=%u\n",
                m->op, id, ok, m->pay_len);
    pthread_mutex_lock(&p->rq_lk);
    uint32_t expect = p->rq_op == LMB_RREAD_FWD ? LMB_RREAD_R :
                      p->rq_op == LMB_REXEC_FWD ? LMB_REXEC_R : LMB_RSEG_R;
    if (p->rq_busy && p->rq_sent && id == p->rq_id && m->op == expect &&
        !p->rq_done) {
        if (expect == LMB_RSEG_R) {
            uint32_t response_op = 0, body_len = 0;
            int valid = ok && !lmb_cur_u32(&c, &response_op) &&
                        !lmb_cur_u32(&c, &body_len) &&
                        body_len == c.len - c.off &&
                        response_op == p->rq_inner_op + 1u &&
                        lmb_frame_shape_ok(response_op, body_len, m->pay_len);
            if (valid) {
                p->rq_resp = malloc(body_len ? body_len : 1u);
                if (p->rq_resp) {
                    if (body_len) memcpy(p->rq_resp, c.p + c.off, body_len);
                    p->rq_resp_len = body_len;
                    p->rq_resp_pay = lmb_msg_take_pay(m);
                    p->rq_resp_pay_len = m->pay_len;
                    p->rq_resp_op = response_op;
                    p->rq_ok = 1;
                }
            }
        } else {
            p->rq_ok = ok && m->pay_len > 0;
            p->rq_resp = lmb_msg_take_pay(m); p->rq_resp_len = m->pay_len;
        }
        p->rq_done = 1;
        pthread_cond_broadcast(&p->rq_cv);
    }
    pthread_mutex_unlock(&p->rq_lk);
}

static void ctrl_teardown(Peer *p, int fd) {
    pthread_mutex_lock(&g_lk);
    if (p->ctrl_fd == fd) p->ctrl_fd = -1;
    pthread_mutex_unlock(&g_lk);
    pthread_mutex_lock(&p->rq_lk);
    if (p->rq_busy && !p->rq_done) { p->rq_done = 1; p->rq_ok = 0; pthread_cond_broadcast(&p->rq_cv); }
    pthread_mutex_unlock(&p->rq_lk);
    pthread_mutex_lock(&g_lk);
    if (p->refs) p->refs--;
    pthread_mutex_unlock(&g_lk);
}

/* ---- connections -------------------------------------------------------- */

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    if (lmb_secure_server(fd)) { close(fd); lmb_conn_gate_leave(&g_conn_gate); return NULL; }
    Peer *ctrl = NULL;    /* set once this connection REGISTERs */
    int authed = g_token[0] ? 0 : 1;
    uint8_t nonce[32]; int have_nonce = 0;   /* per-connection identity challenge */
    for (;;) {
        if (ctrl) {
            struct pollfd pf[2] = { { fd, POLLIN, 0 }, { ctrl->evfd, POLLIN, 0 } };
            int pr = poll(pf, 2, -1);
            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (pf[1].revents & POLLIN) {
                uint64_t junk;
                while (read(ctrl->evfd, &junk, 8) == 8) { }
                LmbBuf b = {0};
                uint8_t *pay = NULL; uint32_t pay_len = 0, op = 0;
                int have = 0;
                pthread_mutex_lock(&ctrl->rq_lk);
                if (ctrl->rq_busy && !ctrl->rq_sent && !ctrl->rq_done) {
                    ctrl->rq_sent = 1; have = 1;
                    op = ctrl->rq_op;
                    lmb_buf_u32(&b, ctrl->rq_id);
                    if (op == LMB_RREAD_FWD) {
                        lmb_buf_str(&b, ctrl->rq_path);
                        lmb_buf_u64(&b, ctrl->rq_off);
                        lmb_buf_u32(&b, ctrl->rq_len);
                    } else if (op == LMB_REXEC_FWD) {
                        lmb_buf_bytes(&b, ctrl->rq_body, ctrl->rq_body_len);
                        pay_len = ctrl->rq_pay_len;
                        if (pay_len) {
                            pay = (uint8_t *)malloc(pay_len);
                            if (pay) memcpy(pay, ctrl->rq_pay, pay_len);
                            else {
                                have = 0; ctrl->rq_done = 1; ctrl->rq_ok = 0;
                                pthread_cond_broadcast(&ctrl->rq_cv);
                            }
                        }
                    } else if (op == LMB_RSEG_FWD) {
                        lmb_buf_u32(&b, ctrl->rq_inner_op);
                        lmb_buf_u32(&b, ctrl->rq_body_len);
                        lmb_buf_bytes(&b, ctrl->rq_body, ctrl->rq_body_len);
                        pay_len = ctrl->rq_pay_len;
                        if (pay_len) {
                            pay = malloc(pay_len);
                            if (pay) memcpy(pay, ctrl->rq_pay, pay_len);
                            else {
                                have = 0; ctrl->rq_done = 1; ctrl->rq_ok = 0;
                                pthread_cond_broadcast(&ctrl->rq_cv);
                            }
                        }
                    } else have = 0;
                }
                pthread_mutex_unlock(&ctrl->rq_lk);
                if (have) {
                    if (getenv("LUMABRI_RELAY_TRACE"))
                        fprintf(stderr, "[tracker] relay forward op=%u id=%u "
                                "body=%zu pay=%u\n", op, ctrl->rq_id, b.len, pay_len);
                    int rc = lmb_send(fd, op, b.p, (uint32_t)b.len, pay, pay_len);
                    free(b.p); free(pay);
                    if (rc) break;
                } else { free(b.p); free(pay); }
            }
            if (!(pf[0].revents & POLLIN)) continue;
        }
        LmbMsg m;
        if (lmb_recv(fd, &m) != 0) break;
        int rc = 0;
        if (!authed && m.op != LMB_AUTH && m.op != LMB_PING) {
            send_err(fd, "this swarm needs an invite token");
            lmb_msg_free(&m);
            break;
        }
        switch (m.op) {
        case LMB_AUTH: {
            char tok[LMB_TOKEN_MAX + 1] = "";
            LmbCur c = { m.body, m.body_len, 0 };
            int bad = lmb_cur_str(&c, tok, sizeof tok) || c.off != c.len;
            if (!bad && (!g_token[0] || lmb_token_equal(tok, g_token))) {
                authed = 1;
                rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            } else { send_err(fd, "bad token"); rc = -1; }
            break;
        }
        case LMB_PING:      rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0); break;
        case LMB_CHALLENGE:
            lmb_random(nonce, sizeof nonce);
            have_nonce = 1;
            rc = lmb_send(fd, LMB_CHALLENGE_R, nonce, sizeof nonce, NULL, 0);
            break;
        case LMB_REGISTER: {
            Peer *p = handle_register(fd, &m, have_nonce ? nonce : NULL);
            if (p) {
                if (!ctrl) {
                    pthread_mutex_lock(&g_lk);
                    p->refs++;
                    pthread_mutex_unlock(&g_lk);
                    ctrl = p;
                } else if (ctrl != p) {
                    pthread_mutex_lock(&g_lk);
                    if (p->ctrl_fd == fd) p->ctrl_fd = -1;
                    pthread_mutex_unlock(&g_lk);
                    send_err(fd, "one maintainer identity per control connection");
                    rc = -1;
                }
            } else rc = -1;
            break;
        }
        case LMB_RREAD_R:
        case LMB_REXEC_R:
        case LMB_RSEG_R:    if (ctrl) relay_complete(ctrl, &m); break;
        case LMB_EREG: {
            Peer *p = handle_ereg(fd, &m, have_nonce ? nonce : NULL);
            if (p) {
                if (!ctrl) {
                    pthread_mutex_lock(&g_lk); p->refs++; pthread_mutex_unlock(&g_lk);
                    ctrl = p;
                } else if (ctrl != p) {
                    pthread_mutex_lock(&g_lk);
                    if (p->ctrl_fd == fd) p->ctrl_fd = -1;
                    pthread_mutex_unlock(&g_lk);
                    send_err(fd, "one expert identity per control connection");
                    rc = -1;
                }
            } else rc = -1;
            break;
        }
        case LMB_EASSIGN:   rc = handle_eassign(fd, &m); break;
        case LMB_EPEERS:    rc = handle_epeers(fd, &m); break;
        case LMB_ECOVER:    rc = handle_ecover(fd, &m); break;
        case LMB_SEG_REGISTER: {
            Peer *p = handle_segment_register(fd, &m,
                                               have_nonce ? nonce : NULL);
            if (p) {
                if (!ctrl) {
                    pthread_mutex_lock(&g_lk); p->refs++; pthread_mutex_unlock(&g_lk);
                    ctrl = p;
                } else if (ctrl != p) {
                    pthread_mutex_lock(&g_lk);
                    if (p->ctrl_fd == fd) p->ctrl_fd = -1;
                    pthread_mutex_unlock(&g_lk);
                    send_err(fd, "one compute identity per control connection");
                    rc = -1;
                }
            } else rc = -1;
            break;
        }
        case LMB_SEG_ROUTES: rc = handle_segment_routes(fd, &m); break;
        case LMB_SEG_ASSIGN: rc = handle_segment_assign(fd, &m); break;
        case LMB_SEG_ASSIGN_RELEASE:
            rc = handle_segment_assign_release(fd, &m); break;
        case LMB_HASHES:    rc = handle_hashes(fd, &m); break;
        case LMB_MODEL_ID:  rc = handle_model_id(fd, &m); break;
        case LMB_PLACEMENT: rc = handle_placement(fd, &m); break;
        case LMB_SWARM:     rc = handle_swarm(fd); break;
        case LMB_SWARM_DETAIL: rc = handle_swarm_detail(fd); break;
        case LMB_RREAD:     rc = handle_rread(fd, &m); break;
        case LMB_REXEC:     rc = handle_rexec(fd, &m); break;
        case LMB_RSEG:      rc = handle_rseg(fd, &m); break;
        case LMB_ASSIGN:    rc = handle_assign(fd, &m); break;
        default:            send_err(fd, "unknown op"); rc = -1; break;
        }
        lmb_msg_free(&m);
        if (rc) break;
    }
    if (ctrl) ctrl_teardown(ctrl, fd);
    close(fd);
    lmb_conn_gate_leave(&g_conn_gate);
    return NULL;
}

int main(int argc, char **argv) {
    int port = 7300;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc) {
            const char *tok = argv[++i];
            if (strlen(tok) > LMB_TOKEN_MAX) {
                fprintf(stderr, "[tracker] --token must be at most %u bytes\n",
                        (unsigned)LMB_TOKEN_MAX);
                return 2;
            }
            snprintf(g_token, sizeof g_token, "%s", tok);
        }
        else if (!strcmp(argv[i], "--pubkey") && i + 1 < argc) {
            const char *spec = argv[++i];
            if (lmb_trust_load_spec(&g_trust, spec)) {
                fprintf(stderr, "[tracker] --pubkey wants a 32-byte hex key, "
                                "a comma list, or a keyring file (max %d)\n",
                        LMB_MAX_TRUST_KEYS);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--peer-bindings") && i + 1 < argc) {
            if (snprintf(g_bindings_path, sizeof g_bindings_path, "%s", argv[++i]) >=
                (int)sizeof g_bindings_path) {
                fprintf(stderr, "[tracker] --peer-bindings path is too long\n");
                return 2;
            }
        }
        else { fprintf(stderr, "usage: %s [--port N] [--token S] [--pubkey FILE] "
                               "[--peer-bindings FILE]\n",
                       argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);   /* a vanished peer must not kill the tracker */
    g_stale_s = (double)lmb_env_int("LUMABRI_STALE_MS", (int)(STALE_S * 1000),
                                    100, 3600000) / 1000.0;
    g_max_names_per_source = lmb_env_int("LUMABRI_MAX_NAMES_PER_SOURCE", 16, 1, MAX_PEERS);
    g_rseg_rate_per_s = lmb_env_int("LUMABRI_RSEG_RATE", 2048, 1, 1000000);
    g_rseg_burst = lmb_env_int("LUMABRI_RSEG_BURST", 4096, 1, 1000000);
    g_rseg_source_concurrency = lmb_env_int("LUMABRI_RSEG_SOURCE_CONCURRENCY",
                                            32, 1, 256);
    g_rseg_queue_ms = lmb_env_int("LUMABRI_RSEG_QUEUE_MS", 2000, 0, 60000);
    if (bindings_load()) {
        fprintf(stderr, "[tracker] cannot load trusted peer bindings from %s\n",
                g_bindings_path[0] ? g_bindings_path : "the state directory");
        return 1;
    }
    if (segment_generation_init())
        fprintf(stderr, "[tracker] warning: cannot reserve a durable Segment "
                        "route generation next to %s; legacy services remain "
                        "available but Segment discovery is disabled\n",
                g_bindings_path);
    else g_segment_generation_ready = 1;
    lmb_conn_gate_init(&g_conn_gate);
    if (lmb_secure_init()) return 1;
    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("[tracker] listen"); return 1; }
    printf("[tracker] listening on :%d\n", port);
    if (g_have_pubkey)
        printf("[tracker] signed swarm: truth accepted from %zu trusted "
               "operator key%s\n", g_trust.n, g_trust.n == 1 ? "" : "s");
    fflush(stdout);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("[tracker] accept"); break; }
        lmb_set_io_timeout(fd, lmb_env_int("LUMABRI_IO_TIMEOUT_MS",
                                           LMB_DEFAULT_IO_TIMEOUT_MS, 100, 3600000));
        if (!lmb_conn_gate_enter(&g_conn_gate)) { close(fd); continue; }
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0)
            pthread_detach(t);
        else { lmb_conn_gate_leave(&g_conn_gate); close(fd); }
    }
    return 0;
}
