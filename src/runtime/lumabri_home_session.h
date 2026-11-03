/* Shared approved-plan control: one monitor owns status reads, while UI and
 * keepalive writes serialize complete frames. No process or terminal owner. */
#ifndef LMB_HOME_SESSION_H
#define LMB_HOME_SESSION_H
#include "lumabri_home.h"
#include "lumabri_inventory.h"
#include "lumabri_cluster.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    LmbSocket fd[LMB_CLUSTER_MAX_NODES];
    LmbHomeOffer offers[LMB_CLUSTER_MAX_NODES];
    LmbHomePhase phase[LMB_CLUSTER_MAX_NODES];
    uint32_t host_port[LMB_CLUSTER_MAX_NODES];
    uint32_t segment_port[LMB_CLUSTER_MAX_NODES];
    uint8_t peer_keys[LMB_CLUSTER_MAX_NODES][32];
    char names[LMB_CLUSTER_MAX_NODES][64], addresses[LMB_CLUSTER_MAX_NODES][64];
    char reason[LMB_CLUSTER_MAX_NODES][160];
    uint32_t count, edge;
    void (*interrupted)(void *);
    void *interrupt_context;
    _Atomic int stop, failed;
    pthread_mutex_t status_lock, send_lock;
} HomeSession;

#define HOME_SESSION_INIT { .status_lock = PTHREAD_MUTEX_INITIALIZER, \
                            .send_lock = PTHREAD_MUTEX_INITIALIZER }

static int home_session_error(HomeSession *s, uint32_t i, const char *operation, int error) {
    char detail[160] = {0};
    /* The transport reports EOF as ECONNRESET rather than leaving errno
     * unset. Preserve the actionable, platform-independent UI category
     * while retaining the actual error number for diagnostics. */
    const char *reason = !error ? "connection closed" :
        (error == ECONNRESET || error == EPIPE) ? "connection closed or reset" : strerror(error);
    snprintf(detail, sizeof detail, "%s at %.64s: %s (errno %d).",
             operation, s->addresses[i], reason, error);
    pthread_mutex_lock(&s->status_lock);
    if (!s->reason[i][0])
        memcpy(s->reason[i], detail, sizeof detail);
    pthread_mutex_unlock(&s->status_lock);
    return -1;
}

/* One complete frame at a time, including plaintext diagnostic fixtures.
 * The preparation UI may send COMMIT while the monitor sends a heartbeat. */
static int home_session_send(HomeSession *s, uint32_t i, uint32_t op) {
    pthread_mutex_lock(&s->send_lock);
    int rc = lmb_send(s->fd[i], op, s->offers[i].id, 32, NULL, 0);
    int error = errno;
    pthread_mutex_unlock(&s->send_lock);
    return rc ? home_session_error(s, i, "Cannot send control message", error) : 0;
}

static void home_session_snapshot(HomeSession *s, LmbHomePhase *phases, uint32_t *ports) {
    pthread_mutex_lock(&s->status_lock);
    memcpy(phases, s->phase, s->count * sizeof *phases);
    memcpy(ports, s->host_port, s->count * sizeof *ports);
    pthread_mutex_unlock(&s->status_lock);
}

static int home_session_receive(HomeSession *s, uint32_t i) {
    LmbMsg m = {0};
    errno = 0;
    if (lmb_recv(s->fd[i], &m)) {
        int error = errno; lmb_msg_free(&m);
        return home_session_error(s, i, "Cannot receive donor status", error);
    }
    LmbCur c = {m.body, m.body_len, 0};
    uint32_t version = 0, phase = 0, segment_port = 0, host_port = 0;
    int bad = m.op != LMB_HOME_STATUS || m.pay_len ||
        lmb_cur_u32(&c, &version) || version != LMB_HOME_VERSION || c.len - c.off < 32;
    if (!bad) { bad = memcmp(c.p + c.off, s->offers[i].id, 32); c.off += 32; }
    char reason[160] = {0};
    if (!bad) bad = lmb_cur_u32(&c, &phase) || phase > LMB_HOME_CLOSED ||
        lmb_cur_u32(&c, &segment_port) || segment_port > 65535 ||
        lmb_cur_u32(&c, &host_port) || host_port > 65535 ||
        lmb_inventory_string(&c, reason, sizeof reason) || c.off != c.len;
    lmb_msg_free(&m);
    if (bad) return home_session_error(s, i, "Invalid donor status", EPROTO);
    pthread_mutex_lock(&s->status_lock);
    s->phase[i] = (LmbHomePhase)phase; s->host_port[i] = host_port;
    s->segment_port[i] = segment_port;
    snprintf(s->reason[i], sizeof s->reason[i], "%s", reason);
    pthread_mutex_unlock(&s->status_lock);
    return phase >= LMB_HOME_REJECTED ? -1 : 0;
}

static int home_session_poll(HomeSession *s, int pulse) {
#ifdef _WIN32
    WSAPOLLFD fds[LMB_CLUSTER_MAX_NODES];
#else
    struct pollfd fds[LMB_CLUSTER_MAX_NODES];
#endif
    for (uint32_t i = 0; i < s->count; i++) {
        fds[i].fd=s->fd[i];fds[i].events=POLLIN;fds[i].revents=0;
        if (pulse && home_session_send(s, i, LMB_HOME_PULSE)) return -1;
    }
#ifdef _WIN32
    int n=WSAPoll(fds,s->count,50);if (n<0) lmb_socket_error();
#else
    int n=poll(fds,s->count,50);
#endif
    if (n < 0 && errno != EINTR) return -1;
    for (uint32_t i = 0; i < s->count; i++)
        if (fds[i].revents && home_session_receive(s, i)) return -1;
    return 0;
}

static void *home_session_keepalive(void *arg) {
    HomeSession *s = arg;
    double pulse = 0;
    while (!atomic_load(&s->stop)) {
        int send_pulse = ((double)lmb_io_monotonic_ms()/1000.0) - pulse >= 1;
        if (send_pulse) pulse = ((double)lmb_io_monotonic_ms()/1000.0);
        if (home_session_poll(s, send_pulse)) {
            atomic_store(&s->failed, 1);
            if (s->interrupted) s->interrupted(s->interrupt_context);
            break;
        }
    }
    return NULL;
}

static void home_session_close(HomeSession *s, int retain_weights) {
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->fd[i] == LMB_INVALID_SOCKET) continue;
        if (retain_weights) {
            if (lmb_send(s->fd[i], LMB_HOME_DETACH, s->offers[i].id, 32, NULL, 0) ||
                home_session_receive(s, i))
                fprintf(stderr, "[resident] Could not confirm retained allocation on %s. Check its donor screen.\n", s->names[i]);
        } else (void)lmb_send(s->fd[i], LMB_HOME_CANCEL, s->offers[i].id, 32, NULL, 0);
#ifdef _WIN32
        shutdown(s->fd[i], SD_BOTH);
#else
        shutdown(s->fd[i], SHUT_RDWR);
#endif
        lmb_close(s->fd[i]); s->fd[i] = LMB_INVALID_SOCKET;
    }
}

static int home_session_hybrid_routes(HomeSession *s) {
    LmbHybridRoutes routes = s->offers[s->edge].hybrid;
    int bad = 0;
    pthread_mutex_lock(&s->status_lock);
    for (uint32_t i = 0; i < routes.count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < s->count; j++) {
            if (memcmp(routes.peers[i].key, s->peer_keys[j], 32)) continue;
            char ip[64]; snprintf(ip, sizeof ip, "%s", s->addresses[j]);
            char *colon = strrchr(ip, ':');
            if (!colon || !s->segment_port[j] || s->phase[j] != LMB_HOME_SEGMENT_READY) { bad = 1; break; }
            *colon = 0;
            int n = snprintf(routes.peers[i].addr, sizeof routes.peers[i].addr, "%s:%u", ip, s->segment_port[j]);
            if (n < 0 || (size_t)n >= sizeof routes.peers[i].addr) bad = 1;
            found = 1; break;
        }
        if (!found) bad = 1;
    }
    pthread_mutex_unlock(&s->status_lock);
    LmbBuf b = {0};
    if (bad || lmb_hybrid_routes_pack(&b, &routes, 1)) { free(b.p); return -1; }
    pthread_mutex_lock(&s->send_lock);
    int rc = lmb_send(s->fd[s->edge], LMB_HOME_HYBRID_ROUTES, b.p, (uint32_t)b.len, NULL, 0);
    pthread_mutex_unlock(&s->send_lock); free(b.p); return rc;
}


#endif
