/* expert_node.c — a lumabri phase-2 peer: it HOLDS experts and EXECUTES them.
 *
 * This is the maintainer of the real design. It never ships expert weights;
 * it receives an activation row, runs that expert, and returns the result.
 * Per call: 4 KB in, 4 KB out, whatever the expert's size on disk.
 *
 * Bit-identity is the whole point, so this file does not re-implement the
 * expert math: it #includes the engine's own source (main neutralised — the
 * same pattern the project's own tests use to reach engine statics) and
 * calls the very same kernels the local path uses, on weights read by the
 * very same loader. Remote and local cannot drift, because they are one
 * source.
 *
 * The engines do not share a shape, so everything engine-specific lives
 * behind one small contract, one header per engine in expert_engines/:
 *
 *   LmbeSlot                one expert's weights, engine-shaped
 *   lmbe_open(dir,cap,bits) load whatever the loader needs; `bits` is the
 *                           engine's expert quantization and must match the
 *                           chatter's or the two hold different weights
 *   lmbe_n_slots()          layer slots that can hold experts — n_layers for
 *                           olmoe, n_layers+1 for colibri (the MTP slot)
 *   lmbe_n_experts/hidden/inter()
 *   lmbe_routed(slot)       0 for a dense layer: it has no experts to hold
 *   lmbe_slot_init/load()   allocate once, then load any expert into it
 *   lmbe_scratch_new/free() per-call buffers
 *   lmbe_apply()            one expert, N rows — the engine's own kernels;
 *                           `w` carries the router weights when the engine
 *                           applies them inside the expert (V4 only)
 *
 * Build one binary per engine: -DLMBE_ENGINE=colibri gives expert_node_glm.
 *
 * Two residency modes, the colibri way:
 *   default        every held expert loaded into RAM at startup
 *   --cache N      experts live on the SSD; an N-slot LRU in RAM holds the
 *                  hot ones, a miss streams the expert from disk with the
 *                  engine's own loader. This is what lets a 16 GB machine
 *                  hold a 500 GB slice: the swarm's aggregate RAM becomes
 *                  one big distributed LRU, and a cold expert costs one
 *                  NVMe read — invisible next to WAN flight time.
 *
 * With --tracker the node advertises itself (EREG heartbeat), so chatters
 * discover it instead of being configured with a peer list. The server that
 * `lumabri serve`s a model runs one of these on the whole model: a fresh
 * swarm bootstraps with the server executing everything, donors that join
 * are discovered and win the calls they are nearest for, and the server
 * stays the replica of last resort.
 *
 *   ./expert_node --model DIR --port 7401 [--layers 0,2,4 | --stride N:OFF]
 *                 [--cache N] [--tracker H:P] [--model-name S] [--name S]
 */
#ifndef LMBE_ENGINE_HEADER
#define LMBE_ENGINE_HEADER "expert_engines/olmoe.h"
#endif
#include LMBE_ENGINE_HEADER

#include "lumabri_proto.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/prctl.h>

#define HEARTBEAT_S 10

/* Network emulation lives in lumabri_proto.h (lmb_emu_delay): shared with
 * the maintainer so phase-1 and phase-2 measurements use one clock. */

/* TEST ONLY — LUMABRI_CORRUPT_PPM makes this node return a wrong result for
 * that fraction of calls, so the spot-check tests can prove that a lying
 * executor is caught. Announced loudly at startup. */
static long g_corrupt_ppm;
static __thread unsigned t_cseed;

static void maybe_corrupt_out(float *out) {
    if (!g_corrupt_ppm) return;
    if (!t_cseed) t_cseed = (unsigned)(uintptr_t)&t_cseed | 1u;
    t_cseed = t_cseed * 1664525u + 1013904223u;
    if (t_cseed % 1000000u < (unsigned)g_corrupt_ppm) out[0] += 1.0f;
}

typedef struct { int layer, eid; LmbeSlot slot; } Held;

/* one LRU slot of the --cache pool */
typedef struct {
    int gid;                    /* -1 = empty */
    int refs, loading;
    uint64_t stamp;
    int allocated;
    LmbeSlot slot;
} CSlot;

static struct {
    int n_slots, n_experts, hidden, inter;
    uint8_t *holds;             /* [n_slots * n_experts] 1 = this node's expert */
    int nholds;
    /* resident mode */
    Held *held; int nheld;
    int *index;                 /* gid → held idx, -1 */
    /* cache mode */
    CSlot *cs; int ncs;
    uint64_t tick;
    pthread_mutex_t c_lk;       /* cache table */
    pthread_cond_t c_cv;
    pthread_mutex_t load_lk;    /* the engine loader is used one call at a time */
    char name[64], model[64], advertise[64], tracker[64], token[128];
    _Atomic uint64_t calls, cold;
    double busy_s;
    pthread_mutex_t stat_lk;
} g = { .c_lk = PTHREAD_MUTEX_INITIALIZER, .load_lk = PTHREAD_MUTEX_INITIALIZER,
        .stat_lk = PTHREAD_MUTEX_INITIALIZER };

/* Plain malloc, not the engine's falloc: not every engine has one (the V4
 * glue links its store, not a colibri model loader) and this buffer is only
 * ever read and written here. */
static float *node_falloc(size_t n) {
    float *p = (float *)malloc(n * sizeof(float));
    if (!p) { fprintf(stderr, "OOM %zu floats\n", n); exit(1); }
    return p;
}

static double nowd(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void send_err(int fd, const char *msg) {
    LmbBuf b = {0};
    lmb_buf_str(&b, msg);
    lmb_send(fd, LMB_ERR, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
}

/* ---- the SSD-streaming LRU ----------------------------------------------
 * acquire pins a slot (refs) so eviction can never pull weights out from
 * under a running matmul; a miss loads with the engine's own loader under
 * load_lk (the loader is engine-internal state, treated as single-threaded).
 * Hot expert: one table walk. Cold expert: one disk read, ~ms on NVMe. */

static CSlot *cache_acquire(int layer, int eid) {
    int gid = layer * g.n_experts + eid;
    pthread_mutex_lock(&g.c_lk);
    for (;;) {
        CSlot *hit = NULL, *victim = NULL;
        for (int i = 0; i < g.ncs; i++)
            if (g.cs[i].gid == gid) { hit = &g.cs[i]; break; }
        if (hit) {
            if (hit->loading) { pthread_cond_wait(&g.c_cv, &g.c_lk); continue; }
            hit->refs++; hit->stamp = ++g.tick;
            pthread_mutex_unlock(&g.c_lk);
            return hit;
        }
        for (int i = 0; i < g.ncs; i++) {
            CSlot *s = &g.cs[i];
            if (s->refs || s->loading) continue;
            if (!victim || s->gid < 0 || s->stamp < victim->stamp) victim = s;
            if (s->gid < 0) break;              /* an empty slot beats any LRU */
        }
        if (!victim) { pthread_cond_wait(&g.c_cv, &g.c_lk); continue; }
        victim->gid = gid; victim->loading = 1; victim->refs = 1;
        victim->stamp = ++g.tick;
        pthread_mutex_unlock(&g.c_lk);

        pthread_mutex_lock(&g.load_lk);
        if (!victim->allocated) { lmbe_slot_init(&victim->slot); victim->allocated = 1; }
        lmbe_slot_load(layer, eid, &victim->slot);
        pthread_mutex_unlock(&g.load_lk);
        atomic_fetch_add(&g.cold, 1);

        pthread_mutex_lock(&g.c_lk);
        victim->loading = 0;
        pthread_cond_broadcast(&g.c_cv);
        pthread_mutex_unlock(&g.c_lk);
        return victim;
    }
}

static void cache_release(CSlot *s) {
    pthread_mutex_lock(&g.c_lk);
    s->refs--;
    pthread_cond_broadcast(&g.c_cv);
    pthread_mutex_unlock(&g.c_lk);
}

/* One request = every row the chatter's layer routed to this expert. It has
 * to be all of them at once: an engine that computes nr rows in one call
 * does not produce the same floats as nr calls of one row, and byte identity
 * with the local path is the entire claim of this project. */
static int handle_exec(int fd, LmbMsg *m) {
    LmbCur cur = { m->body, m->body_len, 0 };
    uint32_t layer, eid, dim, nrows = 1;
    if (lmb_cur_u32(&cur, &layer) || lmb_cur_u32(&cur, &eid) || lmb_cur_u32(&cur, &dim)) {
        send_err(fd, "bad exec header"); return -1;
    }
    lmb_cur_u32(&cur, &nrows);            /* absent in the single-row dialect */
    if (nrows < 1 || nrows > (1u << 16)) { send_err(fd, "bad row count"); return -1; }
    /* Router weights follow the header only when the engine needs them
     * applied HERE — DeepSeek V4 folds the weight in before the down
     * projection, so it is not something the chatter can multiply back. The
     * body length says which dialect this is. */
    const float *rw = NULL;
    if (m->body_len == 16 + (size_t)nrows * sizeof(float))
        rw = (const float *)(m->body + 16);
    else if (m->body_len != 16) { send_err(fd, "bad exec body"); return -1; }
    if ((int)layer >= g.n_slots || (int)eid >= g.n_experts ||
        (int)dim != g.hidden ||
        m->pay_len != (uint32_t)((size_t)nrows * dim * sizeof(float))) {
        send_err(fd, "exec shape mismatch"); return -1;
    }
    int gid = (int)layer * g.n_experts + (int)eid;
    if (!g.holds[gid]) { send_err(fd, "expert not held by this node"); return 0; }

    /* one scratch per connection thread: the kernels are called concurrently */
    static __thread void *scratch;
    static __thread int scratch_rows;
    if (!scratch || scratch_rows < (int)nrows) {
        if (scratch) lmbe_scratch_free(scratch);
        scratch = lmbe_scratch_new((int)nrows);
        scratch_rows = (int)nrows;
    }

    size_t obytes = (size_t)nrows * g.hidden * sizeof(float);
    float *out = node_falloc((size_t)nrows * g.hidden);
    double t0 = nowd();
    if (g.ncs) {
        CSlot *s = cache_acquire((int)layer, (int)eid);
        lmbe_apply(&s->slot, (int)layer, (const float *)m->pay, out, (int)nrows, rw, scratch);
        cache_release(s);
    } else {
        lmbe_apply(&g.held[g.index[gid]].slot, (int)layer,
                   (const float *)m->pay, out, (int)nrows, rw, scratch);
    }
    double dt = nowd() - t0;
    maybe_corrupt_out(out);
    lmb_emu_delay();   /* the reply's flight time, when one is being emulated */
    int rc = lmb_send(fd, LMB_EXEC_R, NULL, 0, out, (uint32_t)obytes);
    free(out);

    atomic_fetch_add(&g.calls, 1);
    pthread_mutex_lock(&g.stat_lk); g.busy_s += dt; pthread_mutex_unlock(&g.stat_lk);
    return rc;
}

static int handle_emanifest(int fd) {
    LmbBuf b = {0};
    lmb_buf_u32(&b, (uint32_t)g.nholds);
    for (int l = 0; l < g.n_slots; l++)
        for (int e = 0; e < g.n_experts; e++)
            if (g.holds[l * g.n_experts + e]) {
                lmb_buf_u32(&b, (uint32_t)l);
                lmb_buf_u32(&b, (uint32_t)e);
            }
    lmb_buf_u32(&b, (uint32_t)g.hidden);
    int rc = lmb_send(fd, LMB_EMANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    int authed = g.token[0] ? 0 : 1;
    LmbMsg m;
    while (lmb_recv(fd, &m) == 0) {
        int rc;
        if (!authed && m.op != LMB_AUTH && m.op != LMB_PING) {
            send_err(fd, "this swarm needs an invite token");
            lmb_msg_free(&m);
            break;
        }
        switch (m.op) {
        case LMB_PING:      lmb_emu_delay();   /* probes must see the distance */
                            rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0); break;
        case LMB_AUTH: {
            char tok[128] = "";
            LmbCur c = { m.body, m.body_len, 0 };
            lmb_cur_str(&c, tok, sizeof tok);
            if (!g.token[0] || !strcmp(tok, g.token)) {
                authed = 1;
                rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            } else { send_err(fd, "bad token"); rc = -1; }
            break;
        }
        case LMB_EXEC:      rc = handle_exec(fd, &m); break;
        case LMB_EMANIFEST: rc = handle_emanifest(fd); break;
        default:            send_err(fd, "unknown op"); rc = -1; break;
        }
        lmb_msg_free(&m);
        if (rc) break;
    }
    close(fd);
    return NULL;
}

/* ---- tracker heartbeat: this is how chatters find us --------------------- */

static void *control_thread(void *arg) {
    (void)arg;
    int warned = 0;
    for (;;) {
        int fd = lmb_connect(g.tracker);
        if (fd >= 0 && lmb_auth(fd)) { close(fd); fd = -1; }
        if (fd < 0) {
            if (!warned)
                fprintf(stderr, "[%s] tracker %s unreachable (will retry)\n",
                        g.name, g.tracker);
            warned = 1;
            sleep(HEARTBEAT_S);
            continue;
        }
        warned = 0;
        struct timeval tv = { HEARTBEAT_S, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (;;) {
            LmbBuf b = {0};
            lmb_buf_str(&b, g.name);
            lmb_buf_str(&b, g.advertise);
            lmb_buf_str(&b, g.model);
            lmb_buf_u32(&b, (uint32_t)g.nholds);
            int rc = lmb_send(fd, LMB_EREG, b.p, (uint32_t)b.len, NULL, 0);
            free(b.p);
            if (rc) break;
            LmbMsg m;
            if (lmb_recv(fd, &m) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            lmb_msg_free(&m);
            sleep(HEARTBEAT_S);
        }
        close(fd);
        sleep(1);
    }
    return NULL;
}

static void *stats_thread(void *arg) {
    (void)arg;
    uint64_t last = 0;
    for (;;) {
        sleep(5);
        uint64_t calls = atomic_load(&g.calls), cold = atomic_load(&g.cold);
        if (calls == last) continue;
        if (g.ncs)
            printf("[%s] %llu exec calls · %llu cold loads · %.1f%% RAM hit\n",
                   g.name, (unsigned long long)calls, (unsigned long long)cold,
                   calls ? 100.0 * (double)(calls - cold) / (double)calls : 0.0);
        else
            printf("[%s] %llu exec calls\n", g.name, (unsigned long long)calls);
        fflush(stdout);
        last = calls;
    }
    return NULL;
}

static int in_list(const int *list, int n, int v) {
    for (int i = 0; i < n; i++) if (list[i] == v) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = NULL;
    int port = 7401, stride = 1, offset = 0, cache = 0, bits = 8;
    int layers[512], nlayers = 0;
    snprintf(g.name, sizeof g.name, "node");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(g.name, sizeof g.name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--model-name") && i + 1 < argc)
            snprintf(g.model, sizeof g.model, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tracker") && i + 1 < argc)
            snprintf(g.tracker, sizeof g.tracker, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--advertise") && i + 1 < argc)
            snprintf(g.advertise, sizeof g.advertise, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bits") && i + 1 < argc) bits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
            sscanf(argv[++i], "%d:%d", &stride, &offset);
        else if (!strcmp(argv[i], "--layers") && i + 1 < argc) {
            char *s = argv[++i], *save = NULL;
            for (char *t = strtok_r(s, ",", &save); t && nlayers < 512; t = strtok_r(NULL, ",", &save))
                layers[nlayers++] = atoi(t);
        } else {
            fprintf(stderr, "usage: %s --model DIR [--port N] [--name S] "
                            "[--model-name S] [--tracker H:P] [--advertise H:P] "
                            "[--cache N] [--bits N] [--stride N:OFF] "
                            "[--layers a,b,c]\n", argv[0]);
            return 2;
        }
    }
    if (!dir) { fprintf(stderr, "--model is required\n"); return 2; }
    if (stride < 1) stride = 1;
    if (cache < 0) cache = 0;
    signal(SIGPIPE, SIG_IGN);
    const char *tok = getenv("LUMABRI_TOKEN");
    if (tok) snprintf(g.token, sizeof g.token, "%s", tok);
    if (!g.model[0]) {                /* default model name: the dir basename */
        char tmp[LMB_PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", dir);
        size_t l = strlen(tmp);
        while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
        const char *base = strrchr(tmp, '/');
        snprintf(g.model, sizeof g.model, "%s", base ? base + 1 : tmp);
    }
    if (!g.advertise[0]) snprintf(g.advertise, sizeof g.advertise, "127.0.0.1:%d", port);

    /* 1 µs of timer slack instead of the default 50: without this a 250 µs
     * emulated LAN would land anywhere up to 300 µs. Process-local, no
     * privileges, nothing outside this peer is affected. */
    if (lmb_emu_active()) prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
    { const char *cp = getenv("LUMABRI_CORRUPT_PPM");
      if (cp && atol(cp) > 0) {
          g_corrupt_ppm = atol(cp);
          printf("[%s] *** TEST MODE: corrupting results at %ld ppm ***\n",
                 g.name, g_corrupt_ppm);
      } }

    lmbe_open(dir, cache, bits);
    g.n_slots   = lmbe_n_slots();
    g.n_experts = lmbe_n_experts();
    g.hidden    = lmbe_hidden();
    g.inter     = lmbe_inter();
    int cells = g.n_slots * g.n_experts, dense = 0;
    g.holds = calloc((size_t)cells, 1);
    for (int l = 0; l < g.n_slots; l++) {
        if (!lmbe_routed(l)) { dense++; continue; }   /* a dense layer holds nothing */
        if (nlayers && !in_list(layers, nlayers, l)) continue;
        for (int e = 0; e < g.n_experts; e++) {
            int gid = l * g.n_experts + e;
            if (gid % stride != offset) continue;
            g.holds[gid] = 1;
            g.nholds++;
        }
    }
    if (!g.nholds) { fprintf(stderr, "[%s] no experts selected\n", g.name); return 1; }
    if (dense)
        printf("[%s] %s: %d of %d layer slots are dense and route nothing\n",
               g.name, lmbe_engine_name(), dense, g.n_slots);

    double t0 = nowd();
    if (cache > 0) {
        /* SSD residency: experts stay on disk, N slots of RAM catch the hot
         * ones. Nothing is preloaded — the first calls warm the cache, the
         * same way the phase-1 mirror warms. */
        if (cache > g.nholds) cache = g.nholds;
        g.ncs = cache;
        g.cs = calloc((size_t)cache, sizeof(CSlot));
        for (int i = 0; i < cache; i++) g.cs[i].gid = -1;
        pthread_cond_init(&g.c_cv, NULL);
        double mb = (double)cache * (3.0 * g.inter * g.hidden) / 1e6;
        printf("[%s] holding %d experts on disk · %d-slot RAM cache (%.0f MB) · "
               "hidden=%d inter=%d\n",
               g.name, g.nholds, cache, mb, g.hidden, g.inter);
    } else {
        g.index = malloc((size_t)cells * sizeof(int));
        for (int i = 0; i < cells; i++) g.index[i] = -1;
        g.held = calloc((size_t)g.nholds, sizeof(Held));
        for (int l = 0; l < g.n_slots; l++)
            for (int e = 0; e < g.n_experts; e++) {
                int gid = l * g.n_experts + e;
                if (!g.holds[gid]) continue;
                Held *h = &g.held[g.nheld];
                h->layer = l; h->eid = e;
                lmbe_slot_init(&h->slot);
                lmbe_slot_load(l, e, &h->slot);
                g.index[gid] = g.nheld++;
            }
        double mb = (double)g.nheld * (3.0 * g.inter * g.hidden) / 1e6;
        printf("[%s] holding %d experts (%.0f MB resident) loaded in %.1fs · "
               "hidden=%d inter=%d\n",
               g.name, g.nheld, mb, nowd() - t0, g.hidden, g.inter);
    }
    fflush(stdout);

    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("listen"); return 1; }
    if (lmb_emu_active())
        printf("[%s] emulated network: rtt %ld us ± %ld, loss %ld ppm (rto %ld us)\n",
               g.name, lmb_emu_rtt_us, lmb_emu_jitter_us, lmb_emu_loss_ppm, lmb_emu_rto_us);
    printf("[%s] serving EXEC on :%d (model %s)%s\n", g.name, port, g.model,
           g.tracker[0] ? " · registered with tracker" : "");
    fflush(stdout);

    pthread_t t;
    if (g.tracker[0]) { pthread_create(&t, NULL, control_thread, NULL); pthread_detach(t); }
    pthread_create(&t, NULL, stats_thread, NULL); pthread_detach(t);

    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0) pthread_detach(t);
        else close(fd);
    }
    return 0;
}
