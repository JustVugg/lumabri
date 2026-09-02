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

/* The engine sets its OpenMP team on the FIRST LINE of its own main(), and
 * this file neutralises main to reach the engine's statics — so the team was
 * never sized and every expert node has been running on OpenMP's default,
 * which is one thread per hyperthread. The engine measured that choice and
 * rejected it: physical cores only, +2.3x on a 16C/32T Zen3 from the thread
 * count alone (omp_tune.h). Call exactly what main() would have called;
 * engines without the header keep whatever they set for themselves. */
#ifdef COLI_OMP_TUNE_H
#define LMBE_TUNE_THREADS() coli_omp_tune_threads("lumabri")
#else
#define LMBE_TUNE_THREADS() ((void)0)
#endif

#include "lumabri_proto.h"
#include "lumabri_machine.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"

#include <omp.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
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
    char name[64], model[64], advertise[64], tracker[64];
    int want_hold;              /* auto/numeric --hold capacity; 0 = no EASSIGN */
    char token[LMB_TOKEN_MAX + 1];
    char profile[LMB_BUILD_PROFILE_MAX];
    uint8_t peer_sk[64], peer_pk[32];   /* identity to the tracker */
    int bits;
    int resident_mode;
    _Atomic uint32_t resident_state;
    uint32_t resident_flags;
    uint64_t resident_bytes, resident_vram_bytes;
    LmbGovernor governor;
    LmbModelIdentity identity; int have_identity;
    pthread_mutex_t identity_lk;
    _Atomic uint64_t calls, cold;
    _Atomic uint32_t in_flight;
    double busy_s;
    pthread_mutex_t stat_lk;
} g = { .c_lk = PTHREAD_MUTEX_INITIALIZER, .load_lk = PTHREAD_MUTEX_INITIALIZER,
        .stat_lk = PTHREAD_MUTEX_INITIALIZER,
        .identity_lk = PTHREAD_MUTEX_INITIALIZER };

static LmbConnGate g_conn_gate = LMB_CONN_GATE_INIT;
static __thread void *t_scratch;
static __thread int t_scratch_rows;

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

static int node_arg_copy(char *dst, size_t cap, const char *src,
                         const char *label) {
    size_t n = strlen(src);
    if (n >= cap) {
        fprintf(stderr, "%s must be at most %zu bytes\n", label, cap - 1);
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

/* ---- the compute gate ---------------------------------------------------
 * The default is not a constant but a division: cores / threads-per-expert.
 * With OMP_NUM_THREADS=1 the teams are single-threaded and the gate opens
 * wide; with a full team per call it narrows to one. Either way the machine
 * is asked for about the number of threads it actually has. */
static pthread_mutex_t g_gate_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_free = 1;
static _Atomic uint64_t g_gate_waits;

static void gate_init(int forced, int per_expert, int phys) {
    if (forced > 0) { g_gate_free = forced; return; }
    g_gate_free = (phys > 0 && per_expert > 0) ? phys / per_expert : 1;
    if (g_gate_free < 1) g_gate_free = 1;
}

static void gate_enter(void) {
    pthread_mutex_lock(&g_gate_lk);
    if (g_gate_free <= 0) atomic_fetch_add(&g_gate_waits, 1);
    while (g_gate_free <= 0) pthread_cond_wait(&g_gate_cv, &g_gate_lk);
    g_gate_free--;
    pthread_mutex_unlock(&g_gate_lk);
}

static void gate_leave(void) {
    pthread_mutex_lock(&g_gate_lk);
    g_gate_free++;
    pthread_cond_signal(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_lk);
}

static void send_err(int fd, const char *msg) {
    LmbBuf b = {0};
    lmb_buf_str(&b, msg);
    lmb_send(fd, LMB_ERR, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
}

static int expert_index_ok(uint32_t layer, uint32_t eid) {
    return g.n_slots > 0 && g.n_experts > 0 &&
           layer < (uint32_t)g.n_slots && eid < (uint32_t)g.n_experts;
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
static int exec_compute(LmbMsg *m, float **outp, uint32_t *out_len) {
    if (!lmb_governor_accepting(&g.governor)) return -2;
    /* Test aid, in the spirit of LUMABRI_RTT_US: a deliberate compute delay
     * lets the relay tests prove concurrency and heartbeat liveness without
     * needing a model slow enough to exhibit them. */
    static int delay_ms = -1;
    if (delay_ms < 0) delay_ms = lmb_env_int("LUMABRI_EXEC_DELAY_MS", 0, 0, 60000);
    if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000u);
    LmbCur cur = { m->body, m->body_len, 0 };
    uint32_t layer, eid, dim, nrows = 1;
    if (lmb_cur_u32(&cur, &layer) || lmb_cur_u32(&cur, &eid) || lmb_cur_u32(&cur, &dim)) {
        return -1;
    }
    lmb_cur_u32(&cur, &nrows);            /* absent in the single-row dialect */
    /* The cap is not decoration. The size check below used to be computed in
     * 32 bits, so on a model with a large hidden size a caller could pick a
     * row count whose byte length wrapped to something small, match it with a
     * tiny payload, and get us to allocate gigabytes and then read from a
     * NULL payload. Everything here is 64-bit now, and a real batch is a
     * prompt's worth of rows — thousands, never tens of thousands. */
    if (nrows < 1 || nrows > LMB_MAX_EXEC_ROWS) {
        return -1;
    }
    /* Router weights follow the header only when the engine needs them
     * applied HERE — DeepSeek V4 folds the weight in before the down
     * projection, so it is not something the chatter can multiply back. The
     * body length says which dialect this is. */
    const float *rw = NULL;
    if (m->body_len == 16 + (uint64_t)nrows * sizeof(float))
        rw = (const float *)(m->body + 16);
    else if (m->body_len != 16) return -1;
    uint64_t want = (uint64_t)nrows * dim * sizeof(float);
    if (!expert_index_ok(layer, eid) || dim != (uint32_t)g.hidden ||
        want > LMB_MAX_PAY || m->pay_len != want) {
        return -1;
    }
    size_t gid = (size_t)layer * (size_t)g.n_experts + eid;
    if (!g.holds[gid]) return -1;

    /* How many experts may be multiplied at once. Every connection gets its
     * own thread and every expert call opens a full OpenMP team, so without a
     * limit four chatters ask for four times the cores the machine has: the
     * box stops multiplying and starts context-switching. The measured cost
     * of that was a tenfold collapse at four clients. Queueing is the cheaper
     * failure — everyone waits a little instead of everyone thrashing. */
    /* Count both work waiting at the compute gate and work currently running.
     * Keep it visible through configured response delays as those calls still
     * consume end-to-end executor capacity from the chatter's perspective. */
    atomic_fetch_add(&g.in_flight, 1);
    /* Bring the weights in BEFORE taking the compute gate: a cold expert is a
     * disk read of tens of milliseconds, and holding the gate across it
     * turned every cache miss into a stall for every other chatter. The gate
     * protects the cores, not the SSD. */
    CSlot *cs = g.ncs ? cache_acquire((int)layer, (int)eid) : NULL;
#ifdef LMBE_STORE_OWNS_RESIDENCY
    lmbe_touch((int)layer, (int)eid);
#endif
    gate_enter();

    /* one scratch per connection thread: the kernels are called concurrently */
    if (!t_scratch || t_scratch_rows < (int)nrows) {
        if (t_scratch) lmbe_scratch_free(t_scratch);
        t_scratch = lmbe_scratch_new((int)nrows);
        t_scratch_rows = (int)nrows;
    }

    size_t obytes = (size_t)want;
    float *out = node_falloc((size_t)nrows * g.hidden);
    double t0 = nowd();
    if (cs) {
        lmbe_apply(&cs->slot, (int)layer, (const float *)m->pay, out, (int)nrows, rw, t_scratch);
        cache_release(cs);
    } else {
        lmbe_apply(&g.held[g.index[gid]].slot, (int)layer,
                   (const float *)m->pay, out, (int)nrows, rw, t_scratch);
    }
    double dt = nowd() - t0;
    gate_leave();
    maybe_corrupt_out(out);
    { const char *slow = getenv("LUMABRI_EXEC_DELAY_MS");
      if (slow && atoi(slow) > 0) usleep((useconds_t)atoi(slow) * 1000u); }
    lmb_emu_delay();   /* the reply's flight time, when one is being emulated */
    atomic_fetch_add(&g.calls, 1);
    atomic_fetch_sub(&g.in_flight, 1);
    pthread_mutex_lock(&g.stat_lk); g.busy_s += dt; pthread_mutex_unlock(&g.stat_lk);
    *outp = out; *out_len = (uint32_t)obytes;
    return 0;
}

static int handle_exec(int fd, LmbMsg *m) {
    float *out = NULL; uint32_t out_len = 0;
    int status = exec_compute(m, &out, &out_len);
    if (status) {
        send_err(fd, status == -2 ? "executor draining under machine pressure" :
                                    "bad exec request or expert not held");
        return -1;
    }
    int rc = lmb_send(fd, LMB_EXEC_R, NULL, 0, out, out_len);
    free(out);
    return rc;
}

/* Relayed compute must not ride the heartbeat thread. One cold expert
 * slower than the tracker's stale window used to knock this node out of
 * coverage MID-COMPUTE: the control loop was busy inside exec_compute, sent
 * no heartbeat, and the tracker dropped the registration while the answer
 * was still being produced. Workers carry the compute; the control loop
 * keeps beating. A bounded ticket count keeps a burst of forwards from
 * oversubscribing the OpenMP pool — when every ticket is taken the forward
 * runs inline, which is the old behaviour and honest backpressure. */
static pthread_mutex_t g_ctrl_wr = PTHREAD_MUTEX_INITIALIZER;
static sem_t g_relay_tickets;

/* The tracker prepends only its correlation id; the remaining body and pay
 * are byte-for-byte the ordinary EXEC request, so direct and relayed compute
 * necessarily enter the same validation and numeric path. */
static int handle_rexec_fwd(int fd, LmbMsg *m) {
    if (m->body_len < 4) return -1;
    uint32_t id = lmb_get32(m->body);
    if (getenv("LUMABRI_RELAY_TRACE"))
        fprintf(stderr, "[%s] relay exec id=%u body=%u pay=%u\n",
                g.name, id, m->body_len - 4, m->pay_len);
    LmbMsg inner = { .op = LMB_EXEC, .body = m->body + 4,
                     .body_len = m->body_len - 4,
                     .pay = m->pay, .pay_len = m->pay_len };
    float *out = NULL; uint32_t out_len = 0;
    int ok = exec_compute(&inner, &out, &out_len) == 0;
    if (getenv("LUMABRI_RELAY_TRACE"))
        fprintf(stderr, "[%s] relay exec id=%u %s out=%u\n",
                g.name, id, ok ? "ok" : "refused", out_len);
    LmbBuf b = {0};
    lmb_buf_u32(&b, id); lmb_buf_u32(&b, ok ? 1u : 0u);
    pthread_mutex_lock(&g_ctrl_wr);
    int rc = lmb_send(fd, LMB_REXEC_R, b.p, (uint32_t)b.len,
                      out, ok ? out_len : 0);
    pthread_mutex_unlock(&g_ctrl_wr);
    free(b.p); free(out);
    return rc;
}

typedef struct { int fd; LmbMsg m; } RelayJob;

static void *relay_worker(void *arg) {
    RelayJob *job = (RelayJob *)arg;
    handle_rexec_fwd(job->fd, &job->m);
    close(job->fd);
    lmb_msg_free(&job->m);
    free(job);
    sem_post(&g_relay_tickets);
    return NULL;
}

/* Move the message into a worker when a ticket is free; run inline when the
 * pool is saturated. Returns what the control loop needs: 0 to keep going. */
static int dispatch_rexec_fwd(int fd, LmbMsg *m) {
    if (sem_trywait(&g_relay_tickets) == 0) {
        RelayJob *job = (RelayJob *)malloc(sizeof *job);
        /* The worker gets its own descriptor to the same socket: the control
         * loop may close and redial while the compute is still running, and
         * a recycled fd NUMBER must never receive a stale reply meant for
         * the old connection. dup() pins the socket itself. */
        if (job) job->fd = dup(fd);
        if (job && job->fd >= 0) {
            job->m = *m;
            memset(m, 0, sizeof *m);   /* ownership moved: loop frees a husk */
            pthread_t worker;
            if (!pthread_create(&worker, NULL, relay_worker, job)) {
                pthread_detach(worker);
                return 0;
            }
            *m = job->m;               /* thread failed: fall back inline */
            close(job->fd);
            free(job);
        } else if (job) {
            free(job);
        }
        sem_post(&g_relay_tickets);
    }
    return handle_rexec_fwd(fd, m);
}

static void node_refresh_identity(void) {
    if (!g.tracker[0] || !g.model[0]) return;
    LmbModelIdentity id;
    if (lmb_model_identity_get(g.tracker, g.model, &id)) return;
    pthread_mutex_lock(&g.identity_lk);
    g.identity = id;
    g.have_identity = 1;
    pthread_mutex_unlock(&g.identity_lk);
}

static void manifest_build(LmbBuf *b) {
    LmbModelIdentity id = {0};
    pthread_mutex_lock(&g.identity_lk);
    int have_id = g.have_identity;
    pthread_mutex_unlock(&g.identity_lk);
    if (!have_id) node_refresh_identity();
    pthread_mutex_lock(&g.identity_lk);
    have_id = g.have_identity;
    if (have_id) id = g.identity;
    pthread_mutex_unlock(&g.identity_lk);
    lmb_buf_u32(b, LMB_EXPERT_MANIFEST_MAGIC);
    lmb_buf_str(b, lmbe_engine_name());
    lmb_buf_str(b, g.profile);
    lmb_buf_str(b, g.model);
    lmb_buf_u32(b, (uint32_t)g.bits);
    lmb_buf_u32(b, have_id ? 1u : 0u);
    if (have_id) {
        lmb_buf_bytes(b, id.root, sizeof id.root);
        lmb_buf_u32(b, id.has_sig ? 1u : 0u);
        if (id.has_sig) lmb_buf_bytes(b, id.sig, sizeof id.sig);
    }
    lmb_buf_u32(b, (uint32_t)g.nholds);
    for (int l = 0; l < g.n_slots; l++)
        for (int e = 0; e < g.n_experts; e++)
            if (g.holds[l * g.n_experts + e]) {
                lmb_buf_u32(b, (uint32_t)l);
                lmb_buf_u32(b, (uint32_t)e);
            }
    lmb_buf_u32(b, (uint32_t)g.hidden);
}

static int handle_eres(int fd) {
    LmbBuf b = {0};
    lmb_buf_u32(&b, g.resident_flags);
    lmb_buf_u32(&b, atomic_load(&g.resident_state));
    int rc = lmb_send(fd, LMB_ERES_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

static int handle_emanifest(int fd) {
    LmbBuf b = {0};
    manifest_build(&b);
    int rc = lmb_send(fd, LMB_EMANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    if (lmb_secure_server(fd)) { close(fd); lmb_conn_gate_leave(&g_conn_gate); return NULL; }
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
            char tok[LMB_TOKEN_MAX + 1] = "";
            LmbCur c = { m.body, m.body_len, 0 };
            int bad = lmb_cur_str(&c, tok, sizeof tok) || c.off != c.len;
            if (!bad && (!g.token[0] || lmb_token_equal(tok, g.token))) {
                authed = 1;
                rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            } else { send_err(fd, "bad token"); rc = -1; }
            break;
        }
        case LMB_EXEC:      rc = handle_exec(fd, &m); break;
        case LMB_EMANIFEST: rc = handle_emanifest(fd); break;
        case LMB_ERES: rc = handle_eres(fd); break;
        default:            send_err(fd, "unknown op"); rc = -1; break;
        }
        lmb_msg_free(&m);
        if (rc) break;
    }
    if (t_scratch) {
        lmbe_scratch_free(t_scratch);
        t_scratch = NULL; t_scratch_rows = 0;
    }
    close(fd);
    lmb_conn_gate_leave(&g_conn_gate);
    return NULL;
}

/* Ask the tracker which experts to hold (capacity `hold`); returns a
 * malloc'd [n_slots*n_experts] bitmap, or NULL when the tracker is old,
 * unreachable, or the reply is malformed. Shared by the startup assignment
 * and the elastic rebalance below. */
static uint8_t *eassign_request(int hold) {
    size_t cells = (size_t)g.n_slots * g.n_experts;
    LmbBuf b = {0};
    lmb_buf_str(&b, g.model);
    lmb_buf_str(&b, g.name);          /* so the tracker can recognise us */
    lmb_buf_u32(&b, (uint32_t)g.n_slots);
    lmb_buf_u32(&b, (uint32_t)g.n_experts);
    lmb_buf_u32(&b, (uint32_t)hold);
    uint8_t *routed = (uint8_t *)calloc((size_t)g.n_slots, 1);
    if (!routed) { free(b.p); return NULL; }
    for (int l = 0; l < g.n_slots; l++) routed[l] = lmbe_routed(l) ? 1 : 0;
    lmb_buf_u32(&b, (uint32_t)g.n_slots);
    lmb_buf_bytes(&b, routed, (size_t)g.n_slots);
    free(routed);
    LmbMsg r = {0};
    int rc = lmb_request(g.tracker, LMB_EASSIGN, b.p, (uint32_t)b.len, &r);
    free(b.p);
    if (rc || r.op != LMB_EASSIGN_R) {
        fprintf(stderr, "[%s] the tracker did not assign experts "
                        "(older tracker?) — keeping what --layers/--stride "
                        "chose\n", g.name);
        lmb_msg_free(&r);
        return NULL;
    }
    LmbCur c = { r.body, r.body_len, 0 };
    uint32_t n = 0;
    uint8_t *assigned = (uint8_t *)calloc(cells ? cells : 1, 1);
    int bad = !assigned || lmb_cur_u32(&c, &n) || n > (uint32_t)hold ||
              (size_t)n > cells || r.pay_len != 0;
    for (uint32_t i = 0; !bad && i < n; i++) {
        uint32_t l, e;
        if (lmb_cur_u32(&c, &l) || lmb_cur_u32(&c, &e) ||
            !expert_index_ok(l, e) || !lmbe_routed((int)l)) { bad = 1; break; }
        assigned[(size_t)l * (size_t)g.n_experts + e] = 1;
    }
    if (!bad && c.off != c.len) bad = 1;
    lmb_msg_free(&r);
    if (bad) {
        fprintf(stderr, "[%s] tracker returned an invalid expert assignment — "
                        "keeping the local selection\n", g.name);
        free(assigned);
        return NULL;
    }
    return assigned;
}

/* ---- elastic hold ---------------------------------------------------------
 * The share adapts to the swarm. Every LUMABRI_REBALANCE_S the node re-asks
 * the tracker; the tracker keeps at most a fixed number of ranked replicas
 * per expert, so as donors join, the surplus copies are released and the
 * freed capacity refills with whatever the swarm still lacks. Dropping is a
 * bitmap clear (a stale chatter gets a refusal and refetches the manifest,
 * exactly as if the node had restarted); loading is lazy — the cache pulls
 * a newly-held expert on its first call. Only meaningful in cache mode,
 * which --hold auto always sets. */
static void *rebalance_thread(void *arg) {
    (void)arg;
    long period = 600;
    const char *e = getenv("LUMABRI_REBALANCE_S");
    if (e && atol(e) >= 10) period = atol(e);
    size_t cells = (size_t)g.n_slots * g.n_experts;
    for (;;) {
        sleep((unsigned)period);
        uint8_t *want = eassign_request(g.want_hold);
        if (!want) continue;
        int add = 0, drop = 0, total = 0;
        for (size_t k = 0; k < cells; k++) {
            if (want[k] && !g.holds[k]) add++;
            if (!want[k] && g.holds[k]) drop++;
            total += want[k] ? 1 : 0;
        }
        if (add || drop) {
            memcpy(g.holds, want, cells);   /* byte stores; exec reads bytes */
            g.nholds = total;
            printf("[%s] rebalance: +%d \xe2\x88\x92%d experts (now %d) — the "
                   "swarm changed shape\n", g.name, add, drop, total);
            fflush(stdout);
        }
        free(want);
    }
    return NULL;
}

/* ---- tracker heartbeat: this is how chatters find us --------------------- */

static int send_ereg(int fd, const uint8_t nonce[32], int send_stats) {
    LmbBuf b = {0};
    int publishing = lmb_governor_accepting(&g.governor);
    lmb_buf_str(&b, g.name);
    lmb_buf_str(&b, g.advertise);
    lmb_buf_str(&b, g.model);
    lmb_buf_u32(&b, publishing ? (uint32_t)g.nholds : 0u);
    size_t cells = (size_t)g.n_slots * g.n_experts;
    size_t nb = (cells + 7) / 8;
    uint8_t *bits = (uint8_t *)calloc(nb ? nb : 1, 1);
    if (!bits) { free(b.p); return -1; }
    for (size_t k = 0; k < cells; k++)
        if (publishing && g.holds[k]) bits[k >> 3] |= (uint8_t)(1u << (k & 7));
    lmb_buf_u32(&b, (uint32_t)nb);
    lmb_buf_bytes(&b, bits, nb);
    free(bits);
    /* Metadata lets a chatter validate relay-only coverage without first
     * reaching the node's advertised inbound address. */
    lmb_buf_u32(&b, LMB_EXPERT_MANIFEST_MAGIC);
    lmb_buf_str(&b, lmbe_engine_name());
    lmb_buf_str(&b, g.profile);
    lmb_buf_u32(&b, (uint32_t)g.bits);
    lmb_buf_u32(&b, (uint32_t)g.hidden);
    lmb_buf_u32(&b, (uint32_t)g.n_slots);
    lmb_buf_u32(&b, (uint32_t)g.n_experts);
    if (send_stats) {
        lmb_buf_u32(&b, LMB_EREG_STATS_MAGIC);
        lmb_buf_u32(&b, LMB_EREG_STATS_VERSION);
        lmb_buf_u32(&b, LMB_EREG_STATS_LENGTH);
        lmb_buf_u64(&b, atomic_load(&g.calls));
        lmb_buf_u32(&b, atomic_load(&g.in_flight));
        lmb_buf_u32(&b, atomic_load(&g.resident_state));
        lmb_buf_u32(&b, g.resident_flags);
        lmb_buf_u32(&b, g.resident_mode ? (uint32_t)g.nholds : 0u);
        lmb_buf_u64(&b, g.resident_bytes);
        lmb_buf_u64(&b, g.resident_vram_bytes);
    }
    /* identity: sign the connection nonce with this machine's peer key */
    uint8_t msg[512], sig[64];
    size_t ml = lmb_peer_auth_msg(nonce, g.name, g.model, g.advertise, msg, sizeof msg);
    lmb_sign(sig, msg, ml, g.peer_sk);
    lmb_buf_peer_auth(&b, g.peer_pk, sig);
    pthread_mutex_lock(&g_ctrl_wr);
    int rc = lmb_send(fd, LMB_EREG, b.p, (uint32_t)b.len, NULL, 0);
    pthread_mutex_unlock(&g_ctrl_wr);
    free(b.p);
    return rc;
}

static void *governor_thread(void *arg) {
    (void)arg;
    LmbGovernorState previous = lmb_governor_state(&g.governor);
    int first = 1;
    for (;;) {
        LmbGovernorState state = lmb_governor_poll(&g.governor);
        atomic_store(&g.resident_state, state == LMB_GOV_ACTIVE ?
                     LMB_EXPERT_STATE_ACTIVE : LMB_EXPERT_STATE_DRAINING);
        if (first || state != previous) {
            uint64_t available = lmb_machine_available_ram();
            fprintf(stderr, "[%s] governor ", g.name);
            if (!first) fprintf(stderr, "%s -> ",
                                lmb_governor_state_name(previous));
            fprintf(stderr, "%s · %s",
                    lmb_governor_state_name(state),
                    state == LMB_GOV_ACTIVE ? "publishing resident experts" :
                                             "draining and advertising zero coverage");
            if (state != LMB_GOV_ACTIVE)
                fprintf(stderr, " · reason: %s",
                        lmb_governor_reason_name(
                            lmb_governor_reason(&g.governor)));
            fprintf(stderr, " · available %.1f GB / reserve %.1f GB · "
                    "resident weights %.1f GB\n",
                    (double)available / 1e9,
                    (double)g.governor.ram_reserve_bytes / 1e9,
                    (double)g.resident_bytes / 1e9);
            first = 0;
            previous = state;
        }
        sleep(1);
    }
    return NULL;
}

/* The tracker binds a name to the first peer key that registers it and
 * refuses every other key from then on (anti-takeover). The reply is an
 * LMB_ERR on the heartbeat connection — which this loop used to ignore, so a
 * node whose name was already taken hammered the tracker forever, invisibly
 * rejected. Rotate to "<name>-2", "-3", … instead: the swarm gets the
 * donation under a free name, and the log says what happened. */
static void ereg_name_rotate(void) {
    static int attempt = 1;
    char base[sizeof g.name];
    snprintf(base, sizeof base, "%s", g.name);
    if (attempt > 1) {                      /* strip the previous "-N" */
        char *dash = strrchr(base, '-');
        if (dash) *dash = 0;
    }
    if (attempt >= 9) {
        fprintf(stderr, "[%s] the tracker refuses every name I try — is the "
                        "bindings table full of stale entries?\n", g.name);
        return;
    }
    attempt++;
    char next[sizeof g.name];
    snprintf(next, sizeof next, "%.56s-%u", base, (unsigned)attempt % 10u);
    fprintf(stderr, "[%s] tracker: name held by another key — retrying as %s\n",
            g.name, next);
    snprintf(g.name, sizeof g.name, "%s", next);
}

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
        uint8_t nonce[32];
        if (lmb_request_challenge(fd, nonce)) { close(fd); sleep(1); continue; }
        struct timeval tv = { HEARTBEAT_S, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        /* Capability state belongs to this tracker connection. The first
         * heartbeat is always legacy, and reconnecting resets negotiation so
         * a node can safely move from a new tracker to an old one. */
        int stats_enabled = 0;
        if (send_ereg(fd, nonce, stats_enabled)) { close(fd); sleep(1); continue; }
        for (;;) {
            LmbMsg m;
            if (lmb_recv(fd, &m) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (send_ereg(fd, nonce, stats_enabled)) break;
                    continue;
                }
                break;
            }
            int rc = 0;
            if (getenv("LUMABRI_RELAY_TRACE"))
                fprintf(stderr, "[%s] ctrl frame op=%u body=%u\n",
                        g.name, m.op, m.body_len);
            if (m.op == LMB_REXEC_FWD) rc = dispatch_rexec_fwd(fd, &m);
            else if (m.op == LMB_TMAN_FWD && m.body_len >= 4) {
                /* the manifest through the tunnel: what makes a NAT-only
                 * donor a first-class replica instead of a bitmap of last
                 * resort */
                uint32_t id = lmb_get32(m.body);
                LmbBuf mb = {0}, hb = {0};
                manifest_build(&mb);
                lmb_buf_u32(&hb, id); lmb_buf_u32(&hb, 1u);
                pthread_mutex_lock(&g_ctrl_wr);
                rc = lmb_send(fd, LMB_TMAN_R, hb.p, (uint32_t)hb.len,
                              mb.p, (uint32_t)mb.len);
                pthread_mutex_unlock(&g_ctrl_wr);
                free(hb.p); free(mb.p);
            }
            else if (m.op == LMB_ERR) {
                LmbCur ec = { m.body, m.body_len, 0 };
                char why[128] = "";
                if (!lmb_cur_str(&ec, why, sizeof why) && strstr(why, "held")) {
                    ereg_name_rotate();
                    lmb_msg_free(&m);
                    break;              /* reconnect and register the new name */
                }
            }
            else if (m.op == LMB_OK && m.body_len == 12 &&
                     lmb_get32(m.body) == LMB_EREG_CAP_MAGIC &&
                     lmb_get32(m.body + 4) == LMB_EREG_CAP_VERSION &&
                     (lmb_get32(m.body + 8) & LMB_EREG_CAP_STATS))
                stats_enabled = 1;
            lmb_msg_free(&m);
            if (rc) break;
            pthread_mutex_lock(&g.identity_lk);
            int need_identity = !g.have_identity;
            pthread_mutex_unlock(&g.identity_lk);
            if (need_identity) node_refresh_identity();
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
        if (g.ncs) {
            /* cold is bumped in cache_acquire, calls when the exec completes,
             * and the two are read without a lock: a call in flight has
             * counted its cold load but not itself, so cold can momentarily
             * exceed calls. Cumulatively it never does (one cold per exec at
             * most), so clamp the difference instead of letting the unsigned
             * subtraction wrap to a monstrous percentage. */
            uint64_t hits = calls > cold ? calls - cold : 0;
            printf("[%s] %llu exec calls · %llu cold loads · %.1f%% RAM hit\n",
                   g.name, (unsigned long long)calls, (unsigned long long)cold,
                   calls ? 100.0 * (double)hits / (double)calls : 0.0);
        } else
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

/* ---- how wide the machine is --------------------------------------------
 * Count physical cores inside this process's allowed CPU set. Reading every
 * host CPU from /proc made a taskset/container report cores it could not use,
 * so the gate still oversubscribed constrained deployments. The pre-reexec
 * value is carried in an internal env var because libgomp may bind the main
 * thread to one place after OMP_PROC_BIND becomes active. */
static int phys_cores(void) {
    const char *saved = getenv("LUMABRI_PHYSICAL_CORES");
    if (saved && atoi(saved) > 0) return atoi(saved);
#if defined(__linux__) && defined(CPU_SETSIZE)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed) == 0) {
        struct { int package, core; } seen[CPU_SETSIZE];
        int n = 0, logical = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (!CPU_ISSET(cpu, &allowed)) continue;
            logical++;
            char path[160];
            int package = -1, core = -1;
            snprintf(path, sizeof path,
                     "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
            FILE *fp = fopen(path, "r");
            if (fp) {
                if (fscanf(fp, "%d", &package) != 1) package = -1;
                fclose(fp);
            }
            snprintf(path, sizeof path,
                     "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
            fp = fopen(path, "r");
            if (fp) {
                if (fscanf(fp, "%d", &core) != 1) core = -1;
                fclose(fp);
            }
            if (package < 0 || core < 0) continue;
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (seen[i].package == package && seen[i].core == core) { dup = 1; break; }
            if (!dup) { seen[n].package = package; seen[n].core = core; n++; }
        }
        if (n > 0) return n;
        if (logical > 0) return logical; /* topology unknown, still honor cpuset */
    }
#endif
    int n = omp_get_num_procs();
    return n > 0 ? n : 0;
}

/* GLM and Inkling set their hot-wait policy in the main() that the adapter
 * deliberately renames away. libgomp reads those variables before main, so
 * reproducing the policy requires the same one-time self re-exec. Engines
 * using omp_tune.h need no re-exec and keep their native helper below. */
static void runtime_prepare(int argc, char **argv) {
    (void)argc;
#ifdef LMBE_NEEDS_HOT_OMP_REEXEC
    if (getenv("COLI_OMP_TUNED") || getenv("COLI_NO_OMP_TUNE")) return;
    /* An explicit bind policy may already have narrowed this main thread in
     * libgomp's constructor. Re-execing that one-core mask would be worse
     * than leaving the user's existing runtime policy untouched. */
    if (getenv("OMP_PROC_BIND")) return;
    int phys = phys_cores();
    if (phys > 0) {
        char s[24]; snprintf(s, sizeof s, "%d", phys);
        setenv("LUMABRI_PHYSICAL_CORES", s, 1);
    }
    setenv("OMP_WAIT_POLICY", "active", 0);
    setenv("GOMP_SPINCOUNT", "200000", 0);
    setenv("KMP_BLOCKTIME", "200", 0);
    setenv("OMP_PROC_BIND", "close", 0);
    setenv("OMP_DYNAMIC", "FALSE", 0);
    setenv("COLI_OMP_TUNED", "1", 1);
#ifdef __linux__
    fprintf(stderr, "[OMP] lumabri: applying the engine hot-thread policy (one re-exec)\n");
    execv("/proc/self/exe", argv);
    perror("[OMP] execv self-reexec failed, running without hot-thread tuning");
#endif
#endif
}

/* Use the same modern-Linux/legacy-WSL calculation as machine, doctor,
 * Segment and the governor. A second /proc parser here used to leave WSL1
 * Expert donors at zero even after the machine profile was corrected. */
static long meminfo_avail_kb(void) {
    uint64_t kb = lmb_machine_available_ram() >> 10;
    if (!kb) return -1;
    return kb > (uint64_t)LONG_MAX ? LONG_MAX : (long)kb;
}

int main(int argc, char **argv) {
    runtime_prepare(argc, argv); /* must precede every other main-side action */
    sem_init(&g_relay_tickets, 0,
             (unsigned)lmb_env_int("LUMABRI_RELAY_WORKERS", 8, 1, 64));
    const char *dir = NULL;
    int port = 7401, stride = 1, offset = 0, cache = 0, bits = 8, hold = 0, hold_auto = 0;
    int resident = 0;
    int layers[512], nlayers = 0, parallel = 0;
    memcpy(g.name, "node", sizeof "node");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            if (node_arg_copy(g.name, sizeof g.name, argv[++i], "--name")) return 2;
        } else if (!strcmp(argv[i], "--model-name") && i + 1 < argc) {
            if (node_arg_copy(g.model, sizeof g.model, argv[++i], "--model-name")) return 2;
        } else if (!strcmp(argv[i], "--tracker") && i + 1 < argc) {
            if (node_arg_copy(g.tracker, sizeof g.tracker, argv[++i], "--tracker")) return 2;
        } else if (!strcmp(argv[i], "--advertise") && i + 1 < argc) {
            if (node_arg_copy(g.advertise, sizeof g.advertise, argv[++i], "--advertise")) return 2;
        } else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--resident")) resident = 1;
        else if (!strcmp(argv[i], "--bits") && i + 1 < argc) bits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) {
            if (!strcmp(argv[i + 1], "auto")) hold_auto = 1; else hold = atoi(argv[i + 1]);
            i++;
        }
        else if (!strcmp(argv[i], "--parallel") && i + 1 < argc) parallel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
            sscanf(argv[++i], "%d:%d", &stride, &offset);
        else if (!strcmp(argv[i], "--layers") && i + 1 < argc) {
            char *s = argv[++i], *save = NULL;
            for (char *t = strtok_r(s, ",", &save); t && nlayers < 512; t = strtok_r(NULL, ",", &save))
                layers[nlayers++] = atoi(t);
        } else {
            fprintf(stderr, "usage: %s --model DIR [--port N] [--name S] "
                            "[--model-name S] [--tracker H:P] [--advertise H:P] "
                            "[--cache N | --resident] [--bits N] [--hold N] [--parallel N] "
                            "[--stride N:OFF] [--layers a,b,c]\n", argv[0]);
            return 2;
        }
    }
    if (!dir) { fprintf(stderr, "--model is required\n"); return 2; }
    if (stride < 1) stride = 1;
    if (cache < 0) cache = 0;
    signal(SIGPIPE, SIG_IGN);
    const char *tok = getenv("LUMABRI_TOKEN");
    if (tok && strlen(tok) > LMB_TOKEN_MAX) {
        fprintf(stderr, "LUMABRI_TOKEN must be at most %u bytes\n",
                (unsigned)LMB_TOKEN_MAX);
        return 2;
    }
    if (tok) snprintf(g.token, sizeof g.token, "%s", tok);
    if (!g.model[0]) {                /* default model name: the dir basename */
        const char *end = dir + strlen(dir);
        while (end > dir + 1 && end[-1] == '/') end--;
        const char *base = end;
        while (base > dir && base[-1] != '/') base--;
        size_t n = (size_t)(end - base);
        if (!n || n >= sizeof g.model) {
            fprintf(stderr, "model directory basename must be 1..%zu bytes; "
                            "use --model-name\n", sizeof g.model - 1);
            return 2;
        }
        memcpy(g.model, base, n);
        g.model[n] = 0;
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

    int runtime_phys = phys_cores();
    LMBE_TUNE_THREADS();
    /* GLM/Inkling do not include omp_tune.h. Clamp their untouched OpenMP
     * default, and any affinity-constrained engine, unless the user chose an
     * explicit team or disabled tuning. */
    if (!getenv("OMP_NUM_THREADS") && !getenv("COLI_NO_OMP_TUNE") &&
        runtime_phys > 0 && omp_get_max_threads() > runtime_phys) {
        int logical = omp_get_max_threads();
        omp_set_num_threads(runtime_phys);
        fprintf(stderr, "[OMP] lumabri: %d physical-core threads instead of %d logical\n",
                runtime_phys, logical);
    }
    /* One expert call is one small matmul (a 12 MB fp4 expert on DeepSeek,
     * a few MB elsewhere); a full-machine OpenMP team spends more time
     * forking than multiplying, and the admission gate then admits exactly
     * ONE expert at a time: 258 serialized calls per DeepSeek token, and two
     * chatters queued behind each other. A small team per call lets several
     * experts run at once, which is what a layer's top-k arrive as. The
     * operator's explicit OMP_NUM_THREADS or --parallel still wins. */
    if (!getenv("OMP_NUM_THREADS") && !getenv("COLI_NO_OMP_TUNE") &&
        parallel <= 0 && runtime_phys >= 4) {
        int team = runtime_phys >= 16 ? 4 : 2;
        omp_set_num_threads(team);
        fprintf(stderr, "[OMP] lumabri: %d-thread team per expert, %d experts "
                        "at a time on %d physical cores\n",
                team, runtime_phys / team, runtime_phys);
    }
    if (resident && cache > 0) {
        fprintf(stderr, "--resident and --cache are mutually exclusive\n");
        return 2;
    }
    if (hold_auto) resident = 1;
    g.resident_mode = resident || cache == 0;
    long governor_reserve_mb = 4096;
    const char *global_reserve = getenv("LUMABRI_RAM_RESERVE_MB");
    const char *expert_reserve = getenv("LUMABRI_EXPERT_RAM_RESERVE_MB");
    if (global_reserve && atol(global_reserve) >= 256)
        governor_reserve_mb = atol(global_reserve);
    if (expert_reserve && atol(expert_reserve) >= 256)
        governor_reserve_mb = atol(expert_reserve);
    lmb_governor_init(&g.governor, (uint64_t)governor_reserve_mb << 20);
    atomic_store(&g.resident_state, LMB_EXPERT_STATE_ASSIGNED);
    lmbe_open(dir, cache, bits);
    g.bits = lmbe_effective_bits(bits);
    lmb_build_profile(g.profile, sizeof g.profile);
    g.n_slots   = lmbe_n_slots();
    g.n_experts = lmbe_n_experts();
    g.hidden    = lmbe_hidden();
    g.inter     = lmbe_inter();
    if (hold_auto) {
        /* Hold as many experts as this machine's free RAM can keep resident, and
         * ask the tracker for exactly that many: it completes the least-covered
         * layers first and then grows second replicas (keep limit 2), so
         * several "donate compute" machines spread across the model AND give
         * every expert a failover copy, instead of every one holding the
         * whole model or none overlapping. */
        long avail_kb = meminfo_avail_kb();
        long reserve_mb = governor_reserve_mb;
        double avail_b = avail_kb > reserve_mb * 1024L
            ? (double)(avail_kb - reserve_mb * 1024L) * 1024.0 : 0.0;
        double ebytes = 3.0 * g.inter * g.hidden;     /* same width the cache MB uses */
#ifdef LMBE_EXPERT_BYTES
        ebytes = (double)LMBE_EXPERT_BYTES();        /* the glue knows its format */
#endif
        int total = 0;
        for (int l = 0; l < g.n_slots; l++) if (lmbe_routed(l)) total += g.n_experts;
        long n = ebytes > 0 ? (long)(avail_b / ebytes) : total;
        if (n < 1) {
            fprintf(stderr, "[%s] RAM available is below the %ld MB system "
                            "reserve; no expert is advertised\n", g.name, reserve_mb);
            return 1;
        }
        if (n > total) n = total;
        hold = (int)n;
        fprintf(stderr, "[%s] auto-hold resident: up to %d experts (~%.1f GB), "
                        "%ld MB reserved for the system; the tracker assigns "
                        "the least-covered slice\n",
                g.name, hold, (double)hold * ebytes / 1e9, reserve_mb);
    }
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
    /* "the server decides", for compute. With --hold N the node states only
     * what it knows about itself — how many experts it can carry — and the
     * tracker answers with the set nobody else covers. --layers/--stride
     * still work and simply opt out of the coordination. */
    if (hold > 0 && g.tracker[0] && !nlayers && stride == 1) {
        size_t cells = (size_t)g.n_slots * g.n_experts;
        LmbBuf b = {0};
        lmb_buf_str(&b, g.model);
        lmb_buf_str(&b, g.name);          /* so the tracker can recognise us */
        lmb_buf_u32(&b, (uint32_t)g.n_slots);
        uint8_t *assigned = eassign_request(hold);
        (void)cells;
        if (assigned) {
            int assigned_n = 0;
            for (size_t k = 0; k < cells; k++) assigned_n += assigned[k] ? 1 : 0;
            memcpy(g.holds, assigned, cells);
            g.nholds = assigned_n;
            free(assigned);
            printf("[%s] the tracker assigned %d experts (asked for %d): "
                   "the least replicated of %s\n",
                   g.name, g.nholds, hold, g.model);
        }
        g.want_hold = hold;             /* the rebalance thread re-asks with this */
    }
    if (!g.nholds) { fprintf(stderr, "[%s] no experts selected\n", g.name); return 1; }
    if (dense)
        printf("[%s] %s: %d of %d layer slots are dense and route nothing\n",
               g.name, lmbe_engine_name(), dense, g.n_slots);

    double t0 = nowd();
    atomic_store(&g.resident_state, LMB_EXPERT_STATE_LOADING);
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
#ifdef LMBE_STORE_OWNS_RESIDENCY
        if (lmbe_resident_prepare(g.holds, g.n_slots, g.n_experts, g.nholds)) {
            fprintf(stderr, "[%s] assigned experts did not become resident; "
                            "the peer will not be advertised\n", g.name);
            return 1;
        }
#endif
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
    if (g.resident_mode) {
        g.resident_bytes = (uint64_t)((double)g.nholds *
                           (3.0 * g.inter * g.hidden));
#ifdef LMBE_STORE_OWNS_RESIDENCY
        g.resident_bytes = lmbe_resident_bytes();
#endif
        g.resident_flags = LMB_EXPERT_RESIDENT_RAM;
    } else {
        g.resident_flags = LMB_EXPERT_DISK_FALLBACK;
    }
    atomic_store(&g.resident_state,
                 lmb_governor_accepting(&g.governor) ?
                 LMB_EXPERT_STATE_ACTIVE : LMB_EXPERT_STATE_DRAINING);
    fflush(stdout);

    /* whatever the engine settled on during lmbe_open */
    node_refresh_identity();
    int phys = runtime_phys, per_expert = omp_get_max_threads();
    if (per_expert < 1) per_expert = 1;
    gate_init(parallel, per_expert, phys);

    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("listen"); return 1; }
    if (lmb_emu_active())
        printf("[%s] emulated network: rtt %ld us ± %ld, loss %ld ppm (rto %ld us)\n",
               g.name, lmb_emu_rtt_us, lmb_emu_jitter_us, lmb_emu_loss_ppm, lmb_emu_rto_us);
    printf("[%s] serving EXEC on :%d (model %s)%s · %d expert%s at a time, "
           "%d thread%s each, %d physical core%s\n", g.name, port, g.model,
           g.tracker[0] ? " · registered with tracker" : "",
           g_gate_free, g_gate_free == 1 ? "" : "s",
           per_expert, per_expert == 1 ? "" : "s",
           phys, phys == 1 ? "" : "s");
    if (!phys)
        printf("[%s] physical core count unknown: one expert at a time "
               "(--parallel N to say otherwise)\n", g.name);
    fflush(stdout);

    if (g.tracker[0]) {
        char kp[512];
        if (lmb_peer_identity(lmb_peer_key_path(kp, sizeof kp), g.peer_sk, g.peer_pk)) {
            fprintf(stderr, "[%s] cannot load or create a peer key at %s\n", g.name, kp);
            return 1;
        }
    }
    pthread_t t;
    lmb_conn_gate_init(&g_conn_gate);
    if (lmb_secure_init()) return 1;
    pthread_create(&t, NULL, governor_thread, NULL); pthread_detach(t);
    if (g.tracker[0]) { pthread_create(&t, NULL, control_thread, NULL); pthread_detach(t); }
    if (g.tracker[0] && g.want_hold > 0 && g.ncs > 0) {
        pthread_create(&t, NULL, rebalance_thread, NULL);
        pthread_detach(t);
    }
    pthread_create(&t, NULL, stats_thread, NULL); pthread_detach(t);

    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        lmb_set_io_timeout(fd, lmb_env_int("LUMABRI_IO_TIMEOUT_MS",
                                           LMB_DEFAULT_IO_TIMEOUT_MS, 100, 3600000));
        if (!lmb_conn_gate_enter(&g_conn_gate)) { close(fd); continue; }
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0) pthread_detach(t);
        else { lmb_conn_gate_leave(&g_conn_gate); close(fd); }
    }
    return 0;
}
