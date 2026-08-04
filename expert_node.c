/* expert_node.c — a lumibri phase-2 peer: it HOLDS experts and EXECUTES them.
 *
 * This is the maintainer of the real design. It never ships expert weights;
 * it receives an activation row, runs that expert, and returns the result.
 * Per call: 4 KB in, 4 KB out, whatever the expert's size on disk.
 *
 * Bit-identity is the whole point, so this file does not re-implement the
 * expert math: it #includes olmoe.c (main neutralised — the same pattern the
 * project's own tests use to reach engine statics) and calls the very same
 * matmul_q / SwiGLU the local path uses, on weights read by the very same
 * loader. Remote and local cannot drift, because they are one source.
 *
 *   ./expert_node --model DIR --port 7401 [--layers 0,2,4 | --stride N:OFF]
 *                 [--name node-a]
 *
 * Without a selector the node holds every expert of the model. --stride N:OFF
 * keeps experts whose global index ≡ OFF (mod N) — the easy way to spread a
 * model across N nodes on one machine for a test.
 */
#define main olmoe_main_unused
#include "../moe-stream/c/olmoe.c"
#undef main

#include "lumibri_proto.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/prctl.h>

#define MAX_HELD 65536

/* ---- network emulation, in userspace, on purpose --------------------------
 * Measuring what a real LAN or a real WAN would cost normally means a netem
 * qdisc, which needs root and changes the host's networking for every process
 * on it. This does the same job inside the peer: it holds the reply for the
 * configured round-trip time before sending it.
 *
 * It is faithful for what we are measuring, because the K experts of a layer
 * already travel on K separate sockets served by K separate threads: the waits
 * overlap exactly as real flight time would, so a layer round still costs one
 * RTT and not K of them.
 *
 * The one thing it does NOT reproduce is packet loss with TCP retransmission.
 * LUMIBRI_LOSS_PPM approximates it the only honest way available here — by
 * paying a retransmission timeout on that fraction of calls — and the report
 * says so, so nobody mistakes the model for the real thing.
 *
 *   LUMIBRI_RTT_US=250 LUMIBRI_JITTER_US=50     ≈ gigabit LAN
 *   LUMIBRI_RTT_US=30000 LUMIBRI_JITTER_US=5000 LUMIBRI_LOSS_PPM=1000  ≈ WAN
 */
static long g_rtt_us = 0, g_jitter_us = 0, g_loss_ppm = 0, g_rto_us = 200000;
static __thread unsigned t_seed = 0;

static void net_delay(void) {
    if (!g_rtt_us && !g_jitter_us && !g_loss_ppm) return;
    if (!t_seed) t_seed = (unsigned)(uintptr_t)&t_seed | 1u;
    long us = g_rtt_us;
    if (g_jitter_us > 0) {
        t_seed = t_seed * 1664525u + 1013904223u;
        us += (long)(t_seed % (unsigned)(2 * g_jitter_us + 1)) - g_jitter_us;
    }
    if (g_loss_ppm > 0) {
        t_seed = t_seed * 1664525u + 1013904223u;
        if (t_seed % 1000000u < (unsigned)g_loss_ppm) us += g_rto_us;
    }
    if (us <= 0) return;
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
}

typedef struct { int layer, eid; Slot slot; } Held;

static struct {
    Model M;
    Held *held; int nheld;
    int *index;                 /* [n_layers * n_experts] → held idx, -1 if absent */
    char name[64];
    _Atomic uint64_t calls;
    double busy_s;
    pthread_mutex_t stat_lk;
} g = { .stat_lk = PTHREAD_MUTEX_INITIALIZER };

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

/* The expert, exactly as olmoe.c's moe() computes it — same kernels, same
 * order. The routing weight and the accumulation stay with the chatter: they
 * are the model's semantics, not the peer's business. */
static void expert_apply(const Slot *e, const float *x, float *out,
                         float *g_buf, float *u_buf) {
    Cfg *c = &g.M.c;
    int D = c->hidden, I = c->inter;
    matmul_q(g_buf, x, e->g, e->gs, D, I);
    matmul_q(u_buf, x, e->u, e->us, D, I);
    for (int i = 0; i < I; i++) { float gv = g_buf[i]; g_buf[i] = (gv / (1.f + expf(-gv))) * u_buf[i]; }
    matmul_q(out, g_buf, e->d, e->ds, I, D);
}

static int handle_exec(int fd, LmbMsg *m) {
    Cfg *c = &g.M.c;
    LmbCur cur = { m->body, m->body_len, 0 };
    uint32_t layer, eid, dim;
    if (lmb_cur_u32(&cur, &layer) || lmb_cur_u32(&cur, &eid) || lmb_cur_u32(&cur, &dim)) {
        send_err(fd, "bad exec header"); return -1;
    }
    if ((int)layer >= c->n_layers || (int)eid >= c->n_experts ||
        (int)dim != c->hidden || m->pay_len != dim * sizeof(float)) {
        send_err(fd, "exec shape mismatch"); return -1;
    }
    int h = g.index[(int)layer * c->n_experts + (int)eid];
    if (h < 0) { send_err(fd, "expert not held by this node"); return 0; }

    float *out = falloc(c->hidden);
    float *gb = falloc(c->inter), *ub = falloc(c->inter);
    double t0 = nowd();
    expert_apply(&g.held[h].slot, (const float *)m->pay, out, gb, ub);
    double dt = nowd() - t0;
    net_delay();     /* the reply's flight time, when one is being emulated */
    int rc = lmb_send(fd, LMB_EXEC_R, NULL, 0, out, (uint32_t)(c->hidden * sizeof(float)));
    free(out); free(gb); free(ub);

    atomic_fetch_add(&g.calls, 1);
    pthread_mutex_lock(&g.stat_lk); g.busy_s += dt; pthread_mutex_unlock(&g.stat_lk);
    return rc;
}

static int handle_emanifest(int fd) {
    LmbBuf b = {0};
    lmb_buf_u32(&b, (uint32_t)g.nheld);
    for (int i = 0; i < g.nheld; i++) {
        lmb_buf_u32(&b, (uint32_t)g.held[i].layer);
        lmb_buf_u32(&b, (uint32_t)g.held[i].eid);
    }
    lmb_buf_u32(&b, (uint32_t)g.M.c.hidden);
    int rc = lmb_send(fd, LMB_EMANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p);
    return rc;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    LmbMsg m;
    while (lmb_recv(fd, &m) == 0) {
        int rc;
        switch (m.op) {
        case LMB_PING:      rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0); break;
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

static int in_list(const int *list, int n, int v) {
    for (int i = 0; i < n; i++) if (list[i] == v) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = NULL;
    int port = 7401, stride = 1, offset = 0;
    int layers[512], nlayers = 0;
    snprintf(g.name, sizeof g.name, "node");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(g.name, sizeof g.name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
            sscanf(argv[++i], "%d:%d", &stride, &offset);
        else if (!strcmp(argv[i], "--layers") && i + 1 < argc) {
            char *s = argv[++i], *save = NULL;
            for (char *t = strtok_r(s, ",", &save); t && nlayers < 512; t = strtok_r(NULL, ",", &save))
                layers[nlayers++] = atoi(t);
        } else {
            fprintf(stderr, "usage: %s --model DIR [--port N] [--name S] "
                            "[--stride N:OFF] [--layers a,b,c]\n", argv[0]);
            return 2;
        }
    }
    if (!dir) { fprintf(stderr, "--model is required\n"); return 2; }
    if (stride < 1) stride = 1;

    { const char *e;
      if ((e = getenv("LUMIBRI_RTT_US")))    g_rtt_us = atol(e);
      if ((e = getenv("LUMIBRI_JITTER_US"))) g_jitter_us = atol(e);
      if ((e = getenv("LUMIBRI_LOSS_PPM")))  g_loss_ppm = atol(e);
      if ((e = getenv("LUMIBRI_RTO_US")))    g_rto_us = atol(e); }
    /* 1 µs of timer slack instead of the default 50: without this a 250 µs
     * emulated LAN would land anywhere up to 300 µs. Process-local, no
     * privileges, nothing outside this peer is affected. */
    if (g_rtt_us || g_jitter_us) prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);

    load_cfg(&g.M.c, dir);
    st_init(&g.M.S, dir);
    Cfg *c = &g.M.c;
    g.index = malloc((size_t)c->n_layers * c->n_experts * sizeof(int));
    for (int i = 0; i < c->n_layers * c->n_experts; i++) g.index[i] = -1;
    g.held = calloc(MAX_HELD, sizeof(Held));

    double t0 = nowd();
    for (int l = 0; l < c->n_layers; l++) {
        if (nlayers && !in_list(layers, nlayers, l)) continue;
        for (int e = 0; e < c->n_experts; e++) {
            int gid = l * c->n_experts + e;
            if (gid % stride != offset) continue;
            if (g.nheld == MAX_HELD) break;
            Held *h = &g.held[g.nheld];
            h->layer = l; h->eid = e;
            slot_ensure_allocated(&g.M, &h->slot);
            load_expert_merged(&g.M, l, e, &h->slot);
            h->slot.eid = e;
            g.index[gid] = g.nheld++;
        }
    }
    double load_s = nowd() - t0;
    double mb = (double)g.nheld * (3.0 * c->inter * c->hidden) / 1e6;
    printf("[%s] holding %d experts (%.0f MB resident) loaded in %.1fs · hidden=%d inter=%d\n",
           g.name, g.nheld, mb, load_s, c->hidden, c->inter);
    fflush(stdout);
    if (!g.nheld) { fprintf(stderr, "[%s] no experts selected\n", g.name); return 1; }

    int lfd = lmb_listen(port);
    if (lfd < 0) { perror("listen"); return 1; }
    if (g_rtt_us || g_jitter_us || g_loss_ppm)
        printf("[%s] emulated network: rtt %ld us ± %ld, loss %ld ppm (rto %ld us)\n",
               g.name, g_rtt_us, g_jitter_us, g_loss_ppm, g_rto_us);
    printf("[%s] serving EXEC on :%d\n", g.name, port); fflush(stdout);

    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd) == 0) pthread_detach(t);
        else close(fd);
    }
    return 0;
}
