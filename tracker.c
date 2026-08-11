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
 * State is in-memory only. A tracker restart costs nothing: every maintainer
 * reconnects within one heartbeat.
 *
 *   ./tracker [--port 7300]
 */
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>

#include "lumabri_proto.h"
#include "lumabri_sha.h"
#include "lumabri_sign.h"

#define MAX_PEERS  64
#define MAX_FILES  4096
#define STALE_S    30.0     /* silent this long → dropped from placements */
#define RELAY_WAIT_S 60     /* chatter-side cap on one relayed read */

typedef struct { char path[LMB_PATH_MAX]; uint64_t size; } PFile;

typedef struct {
    char name[64], addr[64], model[64];
    char engine[64], profile[LMB_BUILD_PROFILE_MAX];
    uint32_t bits, hidden, slots, total_experts;
    uint64_t held_bytes, served_bytes, served_reads;
    PFile *files; uint32_t nfiles;
    double ts;
    int used;
    unsigned refs;              /* control/relay users holding this slot */
    int is_expert;              /* EREG peer: executes experts, holds no files */
    uint8_t pubkey[32];         /* this name's identity, bound on first claim */
    int has_key;
    uint32_t nexperts;
    /* WHICH experts, not just how many: one bit per (slot, expert). Without
     * it the tracker can count executors but cannot tell whether they
     * overlap, and so cannot assign a newcomer the part nobody covers. Sent
     * on the EREG heartbeat; 2.4 KB for a 19456-expert model. */
    uint8_t *ebits; uint32_t enbits;

    /* relay mailbox: one in-flight request per peer, serialized. The
     * chatter thread queues it and waits; the peer's control thread
     * forwards it and completes it. evfd wakes the control thread. */
    int ctrl_fd;                /* the live control connection, -1 if none */
    int evfd;
    pthread_mutex_t rq_lk;
    pthread_cond_t rq_cv;
    int rq_busy, rq_sent, rq_done, rq_ok;
    uint32_t rq_id, rq_next, rq_op;
    char rq_path[LMB_PATH_MAX];
    uint64_t rq_off; uint32_t rq_len;
    uint8_t *rq_body; uint32_t rq_body_len;
    uint8_t *rq_pay; uint32_t rq_pay_len;
    uint8_t *rq_resp; uint32_t rq_resp_len;
} Peer;

static Peer g_peers[MAX_PEERS];
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static int g_known_logged[MAX_PEERS];
static char g_token[128];        /* --token: private swarm, invite required */
static LmbConnGate g_conn_gate = LMB_CONN_GATE_INIT;
static double g_stale_s = STALE_S;

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
                memcmp(g_truth[i].hash, *hash, (size_t)nh * 32)) return 0;
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
        free(p->rq_resp);
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
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert == is_expert &&
            g_peers[i].has_key && !strcmp(g_peers[i].name, name))
            return memcmp(g_peers[i].pubkey, pk, 32) ? -1 : 0;
    return 0;
}

static Peer *handle_register(int fd, LmbMsg *m, const uint8_t *nonce) {
    uint8_t peer_pk[32], peer_sig[64];
    int have_auth = 0;
    size_t blen = peer_auth_strip(m, peer_pk, peer_sig, &have_auth);
    LmbCur c = { m->body, blen, 0 };
    char name[64], addr[64], model[64];
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
            if (fnh[i] && !truth_check(model, files[i].path, files[i].size,
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
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && !g_peers[i].is_expert &&
            !strcmp(g_peers[i].name, name)) { slot = &g_peers[i]; idx = i; break; }
    if (!slot) {
        slot = peer_slot_new(0, &idx);
        fresh = slot != NULL;
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
    /* optional, appended by newer nodes: the bitmap of what it holds */
    uint32_t bl = 0; const uint8_t *bits = NULL;
    if (!lmb_cur_u32(&c, &bl) && bl && bl <= (1u << 20) && c.off + bl <= c.len) {
        bits = c.p + c.off; c.off += bl;
    } else bl = 0;
    uint32_t meta = 0, qbits = 0, hidden = 0, slots = 0, total_experts = 0;
    char engine[64] = "", profile[LMB_BUILD_PROFILE_MAX] = "";
    if (c.off < c.len) {
        if (lmb_cur_u32(&c, &meta) || meta != LMB_EXPERT_MANIFEST_MAGIC ||
            lmb_cur_str(&c, engine, sizeof engine) ||
            lmb_cur_str(&c, profile, sizeof profile) ||
            lmb_cur_u32(&c, &qbits) || lmb_cur_u32(&c, &hidden) ||
            lmb_cur_u32(&c, &slots) || lmb_cur_u32(&c, &total_experts) ||
            c.off != c.len) {
            send_err(fd, "bad ereg metadata"); return NULL;
        }
    }
    Peer *slot = NULL;
    int fresh = 0;
    pthread_mutex_lock(&g_lk);
    if (name_key_ok(name, 1, peer_pk) < 0) {
        pthread_mutex_unlock(&g_lk);
        printf("[tracker] REJECTED: expert %s is already held by another key\n", name);
        fflush(stdout); send_err(fd, "name held by another key");
        return NULL;
    }
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert &&
            !strcmp(g_peers[i].name, name)) { slot = &g_peers[i]; break; }
    if (!slot) {
        slot = peer_slot_new(1, NULL);
        fresh = slot != NULL;
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
        slot->used = 1; slot->is_expert = 1;
        slot->nexperts = nexperts; slot->ts = now_s();
        slot->ctrl_fd = fd;
        snprintf(slot->name, sizeof slot->name, "%s", name);
        snprintf(slot->addr, sizeof slot->addr, "%s", use_addr);
        snprintf(slot->model, sizeof slot->model, "%s", model);
        snprintf(slot->engine, sizeof slot->engine, "%s", engine);
        snprintf(slot->profile, sizeof slot->profile, "%s", profile);
        slot->bits = qbits; slot->hidden = hidden;
        slot->slots = slots; slot->total_experts = total_experts;
        if (bl) {
            uint8_t *nb = (uint8_t *)realloc(slot->ebits, bl);
            if (nb) { slot->ebits = nb; memcpy(slot->ebits, bits, bl);
                      slot->enbits = bl * 8; }
        }
    }
    pthread_mutex_unlock(&g_lk);
    if (!slot) { send_err(fd, "peer table full"); return NULL; }
    if (fresh) {
        printf("[tracker] + expert %s @ %s (%s, %u experts)\n",
               name, use_addr, model, nexperts);
        fflush(stdout);
    }
    if (lmb_send(fd, LMB_OK, NULL, 0, NULL, 0)) {
        pthread_mutex_lock(&g_lk);
        if (slot->ctrl_fd == fd) slot->ctrl_fd = -1;
        pthread_mutex_unlock(&g_lk);
        return NULL;
    }
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
        if (g_peers[i].used && g_peers[i].is_expert &&
            now - g_peers[i].ts <= g_stale_s &&
            (!want[0] || !strcmp(g_peers[i].model, want))) n++;
    lmb_buf_u32(&b, n);
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert &&
            now - g_peers[i].ts <= g_stale_s &&
            (!want[0] || !strcmp(g_peers[i].model, want)))
            lmb_buf_str(&b, g_peers[i].addr);
    pthread_mutex_unlock(&g_lk);
    int rc = lmb_send(fd, LMB_EPEERS_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
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
        if (!q->used || !q->is_expert || q->ctrl_fd < 0 || q->evfd < 0 ||
            now - q->ts > g_stale_s || strcmp(q->model, model) ||
            strcmp(q->engine, engine) || strcmp(q->profile, profile) ||
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
    int has_sig = 0;
    /* an empty model matches any, exactly as PLACEMENT treats it */
    for (int i = 0; i < g_ntruth; i++)
        if ((!model[0] || !strcmp(g_truth[i].model, model)) &&
            !strcmp(g_truth[i].path, path)) {
            nh = g_truth[i].nh;
            size = g_truth[i].size;
            has_sig = g_truth[i].has_sig;
            if (has_sig) memcpy(sig, g_truth[i].sig, 64);
            snprintf(found_model, sizeof found_model, "%s", g_truth[i].model);
            copy = (uint8_t *)malloc((size_t)nh * 32);
            if (copy) memcpy(copy, g_truth[i].hash, (size_t)nh * 32);
            break;
        }
    pthread_mutex_unlock(&g_lk);
    if (!copy) { send_err(fd, "no integrity data"); return 0; }
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
    LmbBuf b = {0};
    double now = now_s();
    pthread_mutex_lock(&g_lk);
    uint32_t live = 0;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && !g_peers[i].is_expert &&
            now - g_peers[i].ts <= g_stale_s) live++;
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
    pthread_mutex_unlock(&g_lk);
    int rc = lmb_send(fd, LMB_SWARM_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
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
    pthread_mutex_lock(&g_lk);
    double now = now_s();
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (!p->used || !p->is_expert || strcmp(p->model, model) || !p->ebits) continue;
        if (!strcmp(p->name, name)) {                 /* this node, as it was */
            memcpy(mine, p->ebits, ((cells + 7) / 8 < (p->enbits + 7) / 8
                                    ? (cells + 7) / 8 : (p->enbits + 7) / 8));
            continue;
        }
        if (now - p->ts > g_stale_s) continue;
        for (size_t k = 0; k < cells && k < p->enbits; k++)
            if (p->ebits[k >> 3] & (1u << (k & 7))) cnt[k]++;
    }
    pthread_mutex_unlock(&g_lk);

    /* pass 1: keep what it already had — no churn, no re-download */
    uint32_t n = 0;
    for (size_t k = 0; k < cells && n < capacity; k++) {
        if (!routed[k / nexp]) continue;
        if (mine[k >> 3] & (1u << (k & 7))) {
            pick[n++] = (uint32_t)k;
            cnt[k] = 0xffff;                     /* taken: never picked twice */
        }
    }
    /* pass 2: fill the rest with the least-replicated, rarest first */
    for (uint16_t want = 0; want < 64 && n < capacity; want++)
        for (size_t k = 0; k < cells && n < capacity; k++) {
            if (!routed[k / nexp] || cnt[k] != want) continue;
            pick[n++] = (uint32_t)k;
            cnt[k] = 0xffff;
        }

    LmbBuf b = {0};
    lmb_buf_u32(&b, n);
    for (uint32_t i = 0; i < n; i++) {
        lmb_buf_u32(&b, pick[i] / nexp);
        lmb_buf_u32(&b, pick[i] % nexp);
    }
    free(cnt); free(pick); free(mine);
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
        if (!q->used || !q->is_expert || now - q->ts > g_stale_s ||
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
    uint32_t expect = p->rq_op == LMB_RREAD_FWD ? LMB_RREAD_R : LMB_REXEC_R;
    if (p->rq_busy && p->rq_sent && id == p->rq_id && m->op == expect &&
        !p->rq_done) {
        p->rq_ok = ok && m->pay_len > 0;
        p->rq_resp = m->pay; p->rq_resp_len = m->pay_len;
        m->pay = NULL;                        /* stolen */
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
            char tok[128] = "";
            LmbCur c = { m.body, m.body_len, 0 };
            lmb_cur_str(&c, tok, sizeof tok);
            if (!g_token[0] || !strcmp(tok, g_token)) {
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
        case LMB_REXEC_R:   if (ctrl) relay_complete(ctrl, &m); break;
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
        case LMB_HASHES:    rc = handle_hashes(fd, &m); break;
        case LMB_MODEL_ID:  rc = handle_model_id(fd, &m); break;
        case LMB_PLACEMENT: rc = handle_placement(fd, &m); break;
        case LMB_SWARM:     rc = handle_swarm(fd); break;
        case LMB_RREAD:     rc = handle_rread(fd, &m); break;
        case LMB_REXEC:     rc = handle_rexec(fd, &m); break;
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
        else if (!strcmp(argv[i], "--token") && i + 1 < argc)
            snprintf(g_token, sizeof g_token, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--pubkey") && i + 1 < argc) {
            const char *spec = argv[++i];
            if (lmb_trust_load_spec(&g_trust, spec)) {
                fprintf(stderr, "[tracker] --pubkey wants a 32-byte hex key, "
                                "a comma list, or a keyring file (max %d)\n",
                        LMB_MAX_TRUST_KEYS);
                return 2;
            }
        }
        else { fprintf(stderr, "usage: %s [--port N] [--token S] [--pubkey FILE]\n",
                       argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);   /* a vanished peer must not kill the tracker */
    g_stale_s = (double)lmb_env_int("LUMABRI_STALE_MS", (int)(STALE_S * 1000),
                                    100, 3600000) / 1000.0;
    lmb_conn_gate_init(&g_conn_gate);
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
