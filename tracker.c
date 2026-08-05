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

#define MAX_PEERS  64
#define MAX_FILES  4096
#define STALE_S    30.0     /* silent this long → dropped from placements */
#define RELAY_WAIT_S 60     /* chatter-side cap on one relayed read */

typedef struct { char path[LMB_PATH_MAX]; uint64_t size; } PFile;

typedef struct {
    char name[64], addr[64], model[64];
    uint64_t held_bytes, served_bytes, served_reads;
    PFile *files; uint32_t nfiles;
    double ts;
    int used;
    int is_expert;              /* EREG peer: executes experts, holds no files */
    uint32_t nexperts;

    /* relay mailbox: one in-flight request per peer, serialized. The
     * chatter thread queues it and waits; the peer's control thread
     * forwards it and completes it. evfd wakes the control thread. */
    int ctrl_fd;                /* the live control connection, -1 if none */
    int evfd;
    pthread_mutex_t rq_lk;
    pthread_cond_t rq_cv;
    int rq_busy, rq_sent, rq_done, rq_ok;
    uint32_t rq_id, rq_next;
    char rq_path[LMB_PATH_MAX];
    uint64_t rq_off; uint32_t rq_len;
    uint8_t *rq_resp; uint32_t rq_resp_len;
} Peer;

static Peer g_peers[MAX_PEERS];
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static int g_known_logged[MAX_PEERS];
static char g_token[128];        /* --token: private swarm, invite required */

/* Ground truth: sha256 per LMB_HASH_CHUNK of every (model, path), taken
 * from the FIRST registrant — the origin server registers before any donor
 * exists. A later registrant whose hashes disagree is announcing poison:
 * that file is stripped from its offer and the lie is logged. */
typedef struct {
    char model[64], path[LMB_PATH_MAX];
    uint32_t nh;
    uint8_t *hash;
} GTruth;
static GTruth g_truth[MAX_FILES];
static int g_ntruth;

/* under g_lk; steals *hash on first sight. Returns 1 ok / 0 poison. */
static int truth_check(const char *model, const char *path,
                       uint32_t nh, uint8_t **hash) {
    for (int i = 0; i < g_ntruth; i++)
        if (!strcmp(g_truth[i].model, model) && !strcmp(g_truth[i].path, path))
            return g_truth[i].nh == nh &&
                   memcmp(g_truth[i].hash, *hash, (size_t)nh * 32) == 0;
    if (g_ntruth == MAX_FILES) return 1;      /* table full: cannot judge */
    GTruth *t = &g_truth[g_ntruth++];
    snprintf(t->model, sizeof t->model, "%s", model);
    snprintf(t->path, sizeof t->path, "%s", path);
    t->nh = nh;
    t->hash = *hash;
    *hash = NULL;
    return 1;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void send_err(int fd, const char *msg) {
    LmbBuf b = {0};
    lmb_buf_str(&b, msg);
    lmb_send(fd, LMB_ERR, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
}

/* ---- REGISTER ----------------------------------------------------------- */

static Peer *handle_register(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char name[64], addr[64], model[64];
    uint64_t held, sbytes, sreads;
    uint32_t n;
    if (lmb_cur_str(&c, name, sizeof name) || lmb_cur_str(&c, addr, sizeof addr) ||
        lmb_cur_str(&c, model, sizeof model) || lmb_cur_u64(&c, &held) ||
        lmb_cur_u64(&c, &sbytes) || lmb_cur_u64(&c, &sreads) ||
        lmb_cur_u32(&c, &n) || n > MAX_FILES) { send_err(fd, "bad register"); return NULL; }
    PFile *files = (PFile *)calloc(n ? n : 1, sizeof *files);
    if (!files) { send_err(fd, "oom"); return NULL; }
    for (uint32_t i = 0; i < n; i++)
        if (lmb_cur_str(&c, files[i].path, sizeof files[i].path) ||
            lmb_cur_u64(&c, &files[i].size)) {
            free(files); send_err(fd, "bad register entry"); return NULL;
        }
    /* optional integrity section (older peers simply do not send it) */
    uint8_t **fh = (uint8_t **)calloc(n ? n : 1, sizeof *fh);
    uint32_t *fnh = (uint32_t *)calloc(n ? n : 1, 4);
    size_t save = c.off;
    uint32_t hm = 0;
    int have_h = fh && fnh && !lmb_cur_u32(&c, &hm) && hm == LMB_HASH_MAGIC;
    if (have_h) {
        for (uint32_t i = 0; i < n; i++) {
            if (lmb_cur_u32(&c, &fnh[i]) || fnh[i] > LMB_MAX_BODY / 32) { have_h = 0; break; }
            if (!fnh[i]) continue;
            fh[i] = (uint8_t *)malloc((size_t)fnh[i] * 32);
            if (!fh[i] || lmb_cur_bytes(&c, fh[i], (size_t)fnh[i] * 32)) { have_h = 0; break; }
        }
    } else c.off = save;

    Peer *slot = NULL;
    int idx = -1, fresh = 0;
    pthread_mutex_lock(&g_lk);
    if (have_h) {
        /* poison dies here: a file whose announced hashes contradict the
         * swarm's ground truth is stripped from this peer's offer */
        for (uint32_t i = 0; i < n; ) {
            if (fnh[i] && !truth_check(model, files[i].path, fnh[i], &fh[i])) {
                printf("[tracker] POISON: %s announces different bytes for "
                       "%s/%s — file rejected\n", name, model, files[i].path);
                fflush(stdout);
                free(fh[i]);
                fh[i] = fh[n - 1]; fnh[i] = fnh[n - 1];
                files[i] = files[n - 1];
                n--;
            } else i++;
        }
    }
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && !g_peers[i].is_expert &&
            !strcmp(g_peers[i].name, name)) { slot = &g_peers[i]; idx = i; break; }
    if (!slot)
        for (int i = 0; i < MAX_PEERS; i++)
            if (!g_peers[i].used) {
                slot = &g_peers[i]; idx = i; fresh = 1;
                memset(slot, 0, sizeof *slot);
                slot->ctrl_fd = -1;
                slot->evfd = eventfd(0, EFD_NONBLOCK);
                pthread_mutex_init(&slot->rq_lk, NULL);
                pthread_cond_init(&slot->rq_cv, NULL);
                break;
            }
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
    free(fh); free(fnh);
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

static int handle_ereg(int fd, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    char name[64], addr[64], model[64];
    uint32_t nexperts;
    if (lmb_cur_str(&c, name, sizeof name) || lmb_cur_str(&c, addr, sizeof addr) ||
        lmb_cur_str(&c, model, sizeof model) || lmb_cur_u32(&c, &nexperts)) {
        send_err(fd, "bad ereg"); return -1;
    }
    Peer *slot = NULL;
    int fresh = 0;
    pthread_mutex_lock(&g_lk);
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert &&
            !strcmp(g_peers[i].name, name)) { slot = &g_peers[i]; break; }
    if (!slot)
        for (int i = 0; i < MAX_PEERS; i++)
            if (!g_peers[i].used) {
                slot = &g_peers[i]; fresh = 1;
                memset(slot, 0, sizeof *slot);
                slot->ctrl_fd = -1;
                slot->evfd = -1;
                pthread_mutex_init(&slot->rq_lk, NULL);
                pthread_cond_init(&slot->rq_cv, NULL);
                break;
            }
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
        snprintf(slot->name, sizeof slot->name, "%s", name);
        snprintf(slot->addr, sizeof slot->addr, "%s", use_addr);
        snprintf(slot->model, sizeof slot->model, "%s", model);
    }
    pthread_mutex_unlock(&g_lk);
    if (!slot) { send_err(fd, "peer table full"); return -1; }
    if (fresh) {
        printf("[tracker] + expert %s @ %s (%s, %u experts)\n",
               name, use_addr, model, nexperts);
        fflush(stdout);
    }
    return lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
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
            now - g_peers[i].ts <= STALE_S &&
            (!want[0] || !strcmp(g_peers[i].model, want))) n++;
    lmb_buf_u32(&b, n);
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].used && g_peers[i].is_expert &&
            now - g_peers[i].ts <= STALE_S &&
            (!want[0] || !strcmp(g_peers[i].model, want)))
            lmb_buf_str(&b, g_peers[i].addr);
    pthread_mutex_unlock(&g_lk);
    int rc = lmb_send(fd, LMB_EPEERS_R, b.p, (uint32_t)b.len, NULL, 0);
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
    uint8_t *copy = NULL;
    /* an empty model matches any, exactly as PLACEMENT treats it */
    for (int i = 0; i < g_ntruth; i++)
        if ((!model[0] || !strcmp(g_truth[i].model, model)) &&
            !strcmp(g_truth[i].path, path)) {
            nh = g_truth[i].nh;
            copy = (uint8_t *)malloc((size_t)nh * 32);
            if (copy) memcpy(copy, g_truth[i].hash, (size_t)nh * 32);
            break;
        }
    pthread_mutex_unlock(&g_lk);
    if (!copy) { send_err(fd, "no integrity data"); return 0; }
    LmbBuf b = {0};
    lmb_buf_u32(&b, LMB_HASH_CHUNK);
    lmb_buf_u32(&b, nh);
    int rc = lmb_send(fd, LMB_HASHES_R, b.p, (uint32_t)b.len, copy, nh * 32);
    free(b.p); free(copy);
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
        if (!g_peers[p].used || now - g_peers[p].ts > STALE_S) continue;
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
            now - g_peers[i].ts <= STALE_S) live++;
    lmb_buf_u32(&b, live);
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (!p->used || p->is_expert || now - p->ts > STALE_S) continue;
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
        if (!q->used || now - q->ts > STALE_S || q->ctrl_fd < 0) continue;
        if (model[0] && strcmp(q->model, model)) continue;
        for (uint32_t j = 0; j < q->nfiles; j++)
            if (!strcmp(q->files[j].path, path)) { p = q; break; }
    }
    pthread_mutex_unlock(&g_lk);
    if (!p) { send_err(fd, "no relay-capable peer holds it"); return 0; }

    pthread_mutex_lock(&p->rq_lk);
    while (p->rq_busy) pthread_cond_wait(&p->rq_cv, &p->rq_lk);
    p->rq_busy = 1; p->rq_sent = 0; p->rq_done = 0; p->rq_ok = 0;
    p->rq_id = ++p->rq_next;
    snprintf(p->rq_path, sizeof p->rq_path, "%s", path);
    p->rq_off = off; p->rq_len = len;
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
        if (!q->used || now - q->ts > STALE_S) continue;
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
static void rread_complete(Peer *p, LmbMsg *m) {
    LmbCur c = { m->body, m->body_len, 0 };
    uint32_t id = 0, ok = 0;
    if (lmb_cur_u32(&c, &id) || lmb_cur_u32(&c, &ok)) return;
    pthread_mutex_lock(&p->rq_lk);
    if (p->rq_busy && p->rq_sent && id == p->rq_id && !p->rq_done) {
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
}

/* ---- connections -------------------------------------------------------- */

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    Peer *ctrl = NULL;    /* set once this connection REGISTERs */
    int authed = g_token[0] ? 0 : 1;
    for (;;) {
        if (ctrl) {
            struct pollfd pf[2] = { { fd, POLLIN, 0 }, { ctrl->evfd, POLLIN, 0 } };
            int pr = poll(pf, 2, -1);
            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (pf[1].revents & POLLIN) {
                uint64_t junk;
                while (read(ctrl->evfd, &junk, 8) == 8) { }
                LmbBuf b = {0};
                int have = 0;
                pthread_mutex_lock(&ctrl->rq_lk);
                if (ctrl->rq_busy && !ctrl->rq_sent && !ctrl->rq_done) {
                    ctrl->rq_sent = 1; have = 1;
                    lmb_buf_u32(&b, ctrl->rq_id);
                    lmb_buf_str(&b, ctrl->rq_path);
                    lmb_buf_u64(&b, ctrl->rq_off);
                    lmb_buf_u32(&b, ctrl->rq_len);
                }
                pthread_mutex_unlock(&ctrl->rq_lk);
                if (have) {
                    int rc = lmb_send(fd, LMB_RREAD_FWD, b.p, (uint32_t)b.len, NULL, 0);
                    free(b.p);
                    if (rc) break;
                } else free(b.p);
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
        case LMB_REGISTER: {
            Peer *p = handle_register(fd, &m);
            if (p) ctrl = p; else rc = -1;
            break;
        }
        case LMB_RREAD_R:   if (ctrl) rread_complete(ctrl, &m); break;
        case LMB_EREG:      rc = handle_ereg(fd, &m); break;
        case LMB_EPEERS:    rc = handle_epeers(fd, &m); break;
        case LMB_HASHES:    rc = handle_hashes(fd, &m); break;
        case LMB_PLACEMENT: rc = handle_placement(fd, &m); break;
        case LMB_SWARM:     rc = handle_swarm(fd); break;
        case LMB_RREAD:     rc = handle_rread(fd, &m); break;
        case LMB_ASSIGN:    rc = handle_assign(fd, &m); break;
        default:            send_err(fd, "unknown op"); rc = -1; break;
        }
        lmb_msg_free(&m);
        if (rc) break;
    }
    if (ctrl) ctrl_teardown(ctrl, fd);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = 7300;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc)
            snprintf(g_token, sizeof g_token, "%s", argv[++i]);
        else { fprintf(stderr, "usage: %s [--port N] [--token S]\n", argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);   /* a vanished peer must not kill the tracker */
    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("[tracker] listen"); return 1; }
    printf("[tracker] listening on :%d\n", port); fflush(stdout);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("[tracker] accept"); break; }
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0)
            pthread_detach(t);
        else
            close(fd);
    }
    return 0;
}
