/* Automatic, bounded self-measurement of a sharing computer.
 *
 * Speed: two short kernels, one per role this computer can take. The Segment
 * role streams int8 weight rows through a dot product with the thread count
 * the installed engine would use; the Edge role does a float32 head-style
 * projection. Both run over buffers larger than a cache so that the figure
 * follows memory bandwidth, which is what a resident model is bound by.
 * The result is kept for a week in the private calibration directory and is
 * measured again when the processor, the thread count or the probe changes,
 * or when no valid record exists. It is never run while this computer is
 * computing for a model.
 *
 * Links: one authenticated echo exchange with each other household computer
 * the tracker lists, a few at a time, repeated before it expires. A computer
 * that joins is measured on the next rounds without anyone asking.
 *
 * Neither is a model benchmark. Windows publishes no probe yet. */
#ifndef LUMABRI_MACHINE_PROBE_H
#define LUMABRI_MACHINE_PROBE_H
#include "src/planner/lumabri_machine_speed.h"
#include "src/planner/lumabri_link_evidence.h"

#define LMB_PROBE_VERSION 1u
#define LMB_PROBE_SEGMENT_BYTES (32u << 20)
#define LMB_PROBE_EDGE_BYTES (16u << 20)
#define LMB_PROBE_WIDTH 1024u
#define LMB_PROBE_MIN_SECONDS 0.15
#define LMB_PROBE_MAX_PASSES 64u
#define LMB_PROBE_THREADS_MAX 64u
#define LMB_LINK_REFRESH_SECONDS 240.0
#define LMB_LINK_RETRY_SECONDS 15.0
#define LMB_LINK_LIST_SECONDS 10.0

typedef struct {
    LmbMachineSpeed speed;
    uint64_t key;
    double measured_at;   /* unix seconds; 0 = nothing usable */
} LmbMachineSpeedState;

typedef struct {
    uint8_t peer[32];
    char addr[64];
    LmbLinkEvidence evidence;
    double tried_at;      /* monotonic */
} LmbPeerLink;

typedef struct {
    LmbPeerLink peers[LMB_MACHINE_LINKS_MAX];
    uint32_t count;
    double listed_at;     /* monotonic; 0 = never */
} LmbPeerLinkState;

/* Echo side: the first probe was already received on an authenticated
 * household connection. At most four echoes, no destination, no relay. */
static LMB_UNUSED void lmb_link_probe_serve(LmbSocket fd, LmbMsg *first) {
    LmbMsg *m = first, next = {0};
    lmb_set_io_timeout(fd, 250);
    for (unsigned i = 0; i < 4; i++) {
        int bad = m->op != LMB_LINK_PROBE || m->body_len != 16 || (m->pay_len && m->pay_len != LMB_LINK_BYTES) ||
            lmb_send(fd, LMB_LINK_PROBE_R, m->body, m->body_len, m->pay, m->pay_len);
        if (m == &next) lmb_msg_free(&next);
        if (bad || i == 3) return;
        memset(&next, 0, sizeof next);
        if (lmb_recv(fd, &next)) { lmb_msg_free(&next); return; }
        m = &next;
    }
}

#ifndef _WIN32
#include <pthread.h>
#include <time.h>

static LMB_UNUSED double lmb_probe_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

typedef struct {
    const void *weights; size_t rows; int floats; double sink;
} LmbProbeJob;

static void *lmb_probe_worker(void *arg) {
    LmbProbeJob *job = arg;
    double total = 0;
    if (job->floats) {
        float x[LMB_PROBE_WIDTH];
        for (unsigned i = 0; i < LMB_PROBE_WIDTH; i++) x[i] = (float)((i * 37u) % 17u) - 8.0f;
        const float *w = job->weights;
        for (size_t r = 0; r < job->rows; r++, w += LMB_PROBE_WIDTH) {
            float acc = 0;
            for (unsigned i = 0; i < LMB_PROBE_WIDTH; i++) acc += w[i] * x[i];
            total += acc;
        }
    } else {
        int8_t x[LMB_PROBE_WIDTH];
        for (unsigned i = 0; i < LMB_PROBE_WIDTH; i++) x[i] = (int8_t)((i * 37u) % 17u) - 8;
        const int8_t *w = job->weights;
        for (size_t r = 0; r < job->rows; r++, w += LMB_PROBE_WIDTH) {
            int32_t acc = 0;
            for (unsigned i = 0; i < LMB_PROBE_WIDTH; i++) acc += (int32_t)w[i] * x[i];
            total += acc;
        }
    }
    job->sink = total;
    return NULL;
}

/* Bytes of weights streamed per second by `threads` workers. 0 on failure. */
static LMB_UNUSED uint64_t lmb_probe_rate(size_t bytes, int floats, uint32_t threads) {
    size_t unit = floats ? sizeof(float) : 1, rows = bytes / (unit * LMB_PROBE_WIDTH);
    if (!threads || threads > LMB_PROBE_THREADS_MAX || rows < threads) return 0;
    void *weights = malloc(rows * LMB_PROBE_WIDTH * unit);
    if (!weights) return 0;
    /* Touch every page with non-constant data before the clock starts. */
    uint32_t seed = 0x9e3779b9u;
    if (floats) { float *w = weights;
        for (size_t i = 0; i < rows * LMB_PROBE_WIDTH; i++) { seed = seed * 1664525u + 1013904223u; w[i] = (float)(seed >> 24) / 256.0f - 0.5f; }
    } else { int8_t *w = weights;
        for (size_t i = 0; i < rows * LMB_PROBE_WIDTH; i++) { seed = seed * 1664525u + 1013904223u; w[i] = (int8_t)(seed >> 24); }
    }
    LmbProbeJob jobs[LMB_PROBE_THREADS_MAX]; pthread_t ids[LMB_PROBE_THREADS_MAX];
    size_t share = rows / threads;
    /* The fastest pass, not the mean: a pass interrupted by something else
     * on the computer says nothing about the computer. */
    double started = lmb_probe_clock(), elapsed = 0, sink = 0, fastest = 0; uint32_t passes = 0; int bad = 0;
    while (!bad && passes < LMB_PROBE_MAX_PASSES && elapsed < LMB_PROBE_MIN_SECONDS) {
        uint32_t launched = 0;
        double pass_started = lmb_probe_clock();
        for (uint32_t t = 0; t < threads; t++) {
            jobs[t] = (LmbProbeJob){(const char *)weights + t * share * LMB_PROBE_WIDTH * unit, share, floats, 0};
            if (t + 1 == threads) { (void)lmb_probe_worker(&jobs[t]); continue; }
            if (pthread_create(&ids[t], NULL, lmb_probe_worker, &jobs[t])) { bad = 1; break; }
            launched++;
        }
        for (uint32_t t = 0; t < launched; t++) (void)pthread_join(ids[t], NULL);
        for (uint32_t t = 0; t < threads; t++) sink += jobs[t].sink;
        double pass = lmb_probe_clock() - pass_started;
        if (pass > 0 && (!(fastest > 0) || pass < fastest)) fastest = pass;
        passes++; elapsed = lmb_probe_clock() - started;
    }
    free(weights);
    if (bad || !passes || !(fastest > 0) || !isfinite(sink)) return 0;
    double rate = (double)(share * threads * LMB_PROBE_WIDTH * unit) / fastest;
    return rate >= 1.0 && rate <= 1e12 ? (uint64_t)rate : 0;
}

static LMB_UNUSED int lmb_machine_speed_measure(uint32_t threads, LmbMachineSpeed *out) {
    memset(out, 0, sizeof *out);
    if (!threads) return -1;
    if (threads > LMB_PROBE_THREADS_MAX) threads = LMB_PROBE_THREADS_MAX;
    uint64_t segment = lmb_probe_rate(LMB_PROBE_SEGMENT_BYTES, 0, threads);
    uint64_t edge = lmb_probe_rate(LMB_PROBE_EDGE_BYTES, 1, threads);
    if (!segment && !edge) return -1;
    out->known = (segment ? LMB_SPEED_SEGMENT : 0) | (edge ? LMB_SPEED_EDGE : 0);
    out->segment_bytes_per_s = segment; out->edge_bytes_per_s = edge; out->threads = threads;
    return lmb_machine_speed_valid(out) ? 0 : -1;
}

/* The record is void when anything it depended on changes. */
static LMB_UNUSED uint64_t lmb_machine_speed_key(const LmbMachineProfile *p, uint32_t threads) {
    uint64_t h = UINT64_C(1469598103934665603);
    char text[512];
    int n = snprintf(text, sizeof text, "probe%u|%s|%s|%s|%u|%u|%u|%u", LMB_PROBE_VERSION, p->cpu_model, p->arch,
        p->isa, p->logical_cpus, threads, (unsigned)LMB_PROBE_SEGMENT_BYTES, (unsigned)LMB_PROBE_EDGE_BYTES);
    for (int i = 0; i < n && i < (int)sizeof text; i++) { h ^= (unsigned char)text[i]; h *= UINT64_C(1099511628211); }
    return h ? h : 1;
}

static LMB_UNUSED uint64_t lmb_machine_speed_sum(const char *text) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (; *text; text++) { h ^= (unsigned char)*text; h *= UINT64_C(1099511628211); }
    return h;
}

#define LMB_MACHINE_SPEED_FILE "machine-speed.v1"

static LMB_UNUSED int lmb_machine_speed_load(const char *directory, uint64_t key, uint32_t threads,
    double now, LmbMachineSpeedState *out) {
    memset(out, 0, sizeof *out);
    int dir = lmb_cal_directory(directory, 0);
    if (dir < 0) return -1;
    int fd = openat(dir, LMB_MACHINE_SPEED_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(dir);
    if (fd < 0) return -1;
    char text[256]; struct stat st; ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 077) &&
        st.st_nlink == 1 && st.st_size > 0 && st.st_size < (off_t)sizeof text) n = read(fd, text, sizeof text - 1);
    close(fd);
    if (n <= 0) return -1;
    text[n] = 0;
    char *line = strrchr(text, ' ');
    unsigned long long stored_key, segment, edge, measured, sum; unsigned version, stored_threads, known; char tail;
    if (!line || sscanf(line + 1, "%llx%c", &sum, &tail) != 2 || tail != '\n') return -1;
    *line = 0;
    if (sum != lmb_machine_speed_sum(text) ||
        sscanf(text, "LMB-MSP %u %llx %u %u %llu %llu %llu%c", &version, &stored_key, &stored_threads, &known,
               &segment, &edge, &measured, &tail) != 7 || version != LMB_PROBE_VERSION) return -1;
    LmbMachineSpeed speed = {.known = known, .segment_bytes_per_s = segment, .edge_bytes_per_s = edge,
        .threads = stored_threads};
    if (stored_key != key || stored_threads != threads || !speed.known || !lmb_machine_speed_valid(&speed) ||
        (double)measured > now + 300.0 || now - (double)measured >= LMB_MACHINE_SPEED_TTL_SECONDS) return -1;
    out->speed = speed; out->key = key; out->measured_at = (double)measured;
    return 0;
}

static LMB_UNUSED int lmb_machine_speed_store(const char *directory, const LmbMachineSpeedState *s) {
    if (!lmb_machine_speed_valid(&s->speed) || !s->speed.known) return -1;
    char body[200], text[256];
    snprintf(body, sizeof body, "LMB-MSP %u %llx %u %u %llu %llu %llu", LMB_PROBE_VERSION,
        (unsigned long long)s->key, s->speed.threads, s->speed.known,
        (unsigned long long)s->speed.segment_bytes_per_s, (unsigned long long)s->speed.edge_bytes_per_s,
        (unsigned long long)s->measured_at);
    int length = snprintf(text, sizeof text, "%s %llx\n", body, (unsigned long long)lmb_machine_speed_sum(body));
    int dir = lmb_cal_directory(directory, 1);
    if (dir < 0) return -1;
    (void)unlinkat(dir, LMB_MACHINE_SPEED_FILE ".pending", 0);
    int fd = openat(dir, LMB_MACHINE_SPEED_FILE ".pending", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    int bad = fd < 0 || write(fd, text, (size_t)length) != length || fsync(fd);
    if (fd >= 0 && close(fd)) bad = 1;
    if (!bad && renameat(dir, LMB_MACHINE_SPEED_FILE ".pending", dir, LMB_MACHINE_SPEED_FILE)) bad = 1;
    if (bad) (void)unlinkat(dir, LMB_MACHINE_SPEED_FILE ".pending", 0);
    close(dir);
    return bad ? -1 : 0;
}

/* Fixtures only: LUMABRI_TEST_MACHINE_SPEED="SEGMENT_MIB_S:EDGE_MIB_S" stands
 * in for the probe so that one test host can play computers of different
 * speeds. It is never stored. */
static LMB_UNUSED int lmb_machine_speed_fixture(uint32_t threads, LmbMachineSpeed *out) {
    const char *text = getenv("LUMABRI_TEST_MACHINE_SPEED");
    unsigned long long segment, edge; char tail;
    if (!text || sscanf(text, "%llu:%llu%c", &segment, &edge, &tail) != 2 || !segment || !edge ||
        segment > 900000 || edge > 900000 || !threads) return -1;
    *out = (LmbMachineSpeed){.known = LMB_SPEED_SEGMENT | LMB_SPEED_EDGE, .segment_bytes_per_s = segment << 20,
        .edge_bytes_per_s = edge << 20, .threads = threads > 256 ? 256 : threads};
    return 0;
}

/* Call on every report. Measures only when no valid record exists and the
 * computer is idle; otherwise it only ages what it has. */
static LMB_UNUSED void lmb_machine_speed_refresh(LmbMachineSpeedState *s, const char *directory,
    const LmbMachineProfile *profile, uint32_t threads, int idle) {
    double now = (double)time(NULL);
    if (!threads) threads = profile->logical_cpus;
    if (threads > LMB_PROBE_THREADS_MAX) threads = LMB_PROBE_THREADS_MAX;
    if (!threads) { memset(s, 0, sizeof *s); return; }
    if (!lmb_machine_speed_fixture(threads, &s->speed)) { s->measured_at = now; s->key = 0; return; }
    uint64_t key = lmb_machine_speed_key(profile, threads);
    int current = s->measured_at > 0 && s->key == key && now >= s->measured_at &&
        now - s->measured_at < LMB_MACHINE_SPEED_TTL_SECONDS;
    if (!current && directory && !lmb_machine_speed_load(directory, key, threads, now, s)) current = 1;
    if (!current) {
        memset(s, 0, sizeof *s);
        if (!idle || lmb_machine_speed_measure(threads, &s->speed)) { memset(s, 0, sizeof *s); return; }
        s->key = key; s->measured_at = now;
        if (directory) (void)lmb_machine_speed_store(directory, s);
    }
    s->speed.age_seconds = (uint32_t)(now - s->measured_at);
}

/* Measuring side. The peer's identity comes from the household's inventory
 * and is checked before anything is sent, so the connection neither consults
 * nor extends known_hosts: a measurement leaves no durable trace. */
static LMB_UNUSED int lmb_link_probe_measure(const char *addr, const uint8_t peer[32], LmbLinkEvidence *out) {
    memset(out, 0, sizeof *out);
    LmbSocket fd = lmb_secure_enabled() ? lmb_secure_connect_unpinned(addr, 1000, 1000)
                                        : lmb_connect_ms_io(addr, 1000, 1000);
    if (fd == LMB_INVALID_SOCKET) return -1;
    uint8_t nonce[16], *bytes = malloc(LMB_LINK_BYTES);
    int bad = !bytes || !lmb_secure_peer_matches(fd, peer) || lmb_auth(fd);
    double samples[4] = {0};
    if (!bad) {
        lmb_random(nonce, sizeof nonce);
        for (uint32_t i = 0; i < LMB_LINK_BYTES; i++) bytes[i] = nonce[i % 16] ^ (uint8_t)i;
        lmb_set_io_timeout(fd, 250);
    }
    for (unsigned i = 0; !bad && i < 4; i++) {
        LmbMsg reply = {0}; uint32_t n = i == 3 ? LMB_LINK_BYTES : 0;
        double started = lmb_probe_clock();
        bad = lmb_send(fd, LMB_LINK_PROBE, nonce, sizeof nonce, bytes, n) || lmb_recv(fd, &reply) ||
            reply.op != LMB_LINK_PROBE_R || reply.body_len != sizeof nonce || reply.pay_len != n ||
            memcmp(reply.body, nonce, sizeof nonce) || (n && memcmp(reply.pay, bytes, n));
        samples[i] = lmb_probe_clock() - started;
        lmb_msg_free(&reply);
    }
    free(bytes); lmb_close(fd);
    return bad ? -1 : lmb_link_observed(out, samples, samples[3], (double)time(NULL));
}

/* One round: refresh the peer list now and then, and measure at most two
 * peers, so a report is never delayed by more than two bounded exchanges. */
static LMB_UNUSED void lmb_peer_links_refresh(LmbPeerLinkState *s, const char *tracker, const uint8_t self[32]) {
    double now = lmb_probe_clock();
    if (!s->listed_at || now - s->listed_at >= LMB_LINK_LIST_SECONDS) {
        LmbMachineReport *reports = malloc(sizeof *reports * LMB_INVENTORY_MAX);
        uint32_t n = 0;
        s->listed_at = now;
        if (reports && !lmb_inventory_fetch(tracker, reports, &n)) {
            LmbPeerLinkState next = {0}; next.listed_at = now;
            for (uint32_t i = 0; i < n && next.count < LMB_MACHINE_LINKS_MAX; i++) {
                if (!memcmp(reports[i].identity, self, 32) || !reports[i].control_addr[0]) continue;
                LmbPeerLink *p = &next.peers[next.count++];
                memcpy(p->peer, reports[i].identity, 32);
                snprintf(p->addr, sizeof p->addr, "%s", reports[i].control_addr);
                for (uint32_t j = 0; j < s->count; j++)
                    if (!memcmp(s->peers[j].peer, p->peer, 32) && !strcmp(s->peers[j].addr, p->addr)) {
                        p->evidence = s->peers[j].evidence; p->tried_at = s->peers[j].tried_at;
                    }
            }
            *s = next;
        }
        free(reports);
    }
    double wall = (double)time(NULL);
    for (unsigned round = 0; round < 2; round++) {
        LmbPeerLink *due = NULL;
        for (uint32_t i = 0; i < s->count; i++) {
            LmbPeerLink *p = &s->peers[i];
            int fresh = lmb_link_valid(&p->evidence) && wall >= p->evidence.measured_at &&
                wall - p->evidence.measured_at < LMB_LINK_REFRESH_SECONDS;
            if (fresh || (p->tried_at && now - p->tried_at < LMB_LINK_RETRY_SECONDS)) continue;
            if (!due || p->tried_at < due->tried_at) due = p;
        }
        if (!due) return;
        due->tried_at = now;
        LmbLinkEvidence seen;
        if (!lmb_link_probe_measure(due->addr, due->peer, &seen)) due->evidence = seen;
    }
}

static LMB_UNUSED void lmb_peer_links_export(const LmbPeerLinkState *s, LmbMachineLinks *out) {
    memset(out, 0, sizeof *out);
    double wall = (double)time(NULL);
    for (uint32_t i = 0; i < s->count && out->count < LMB_MACHINE_LINKS_MAX; i++) {
        const LmbLinkEvidence *e = &s->peers[i].evidence;
        if (!lmb_link_valid(e) || wall < e->measured_at || wall - e->measured_at > LMB_MACHINE_LINK_TTL_SECONDS) continue;
        double micros = e->rtt_p50_seconds * 1e6;
        LmbMachineLink *l = &out->links[out->count];
        memcpy(l->peer, s->peers[i].peer, 32);
        l->rtt_micros = micros < 1 ? 1 : micros > 60000000.0 ? 60000000u : (uint32_t)micros;
        l->bytes_per_s = e->echo_bytes_per_second > 1e12 ? UINT64_C(1000000000000) :
            e->echo_bytes_per_second < 1 ? 1 : (uint64_t)e->echo_bytes_per_second;
        l->age_seconds = (uint32_t)(wall - e->measured_at);
        out->count++;
    }
}
#endif /* !_WIN32 */
#endif
