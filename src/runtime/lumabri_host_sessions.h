/* Shared Edge weights, isolated codec slots, and bounded FIFO turn admission.
 * Colibri itself remains single-threaded at Edge: only the turn-gate owner may
 * touch its pipes. A client never chooses the internal slot or reads another
 * client's output. The host owns sockets until their worker has left. */
#ifndef LMB_HOST_SESSIONS_H
#define LMB_HOST_SESSIONS_H

typedef struct HostSessionPool HostSessionPool;
typedef struct {
    HostSessionPool *pool;
    int fd, used, reset_needed;
    uint32_t index;
} HostSessionSlot;

struct HostSessionPool {
    HostState host;
    uint32_t queue_ms;
    LmbRunGate turns;
    pthread_mutex_t lock;
    pthread_cond_t empty;
    HostSessionSlot slots[LMB_HOST_MAX_SESSIONS];
    uint32_t workers;
    _Atomic int stop, failed;
};

static int host_session_cancelled(void *opaque) {
    HostSessionSlot *slot = opaque;
    if (g_stopping || atomic_load(&slot->pool->stop)) return 1;
    /* No pipelined requests while queued. Any incoming frame is either a
     * cancellation or a protocol violation; close just this conversation.
     * Do not block the FIFO gate mutex reading an incomplete network frame. */
    struct pollfd p = {slot->fd, POLLIN, 0};
    int ready = poll(&p, 1, 0);
    return ready > 0 && p.revents;
}

static int host_session_receive(HostSessionSlot *slot, HostState *h, HostInput *in) {
    double deadline = nowd() + h->idle_seconds;
    while (!g_stopping && !atomic_load(&slot->pool->stop) && nowd() < deadline) {
        struct pollfd p = {slot->fd, POLLIN, 0};
        int ready = poll(&p, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (p.revents & (POLLHUP|POLLERR|POLLNVAL))) return -1;
        if (!ready) continue;
        LmbMsg msg = {0};
        int bad = lmb_recv_bounded(slot->fd, &msg, h->max_frame + sizeof in->header, 2000) || msg.op != LMB_HOST_STREAM ||
            msg.body_len || !msg.pay_len || msg.pay_len > h->max_frame + sizeof in->header ||
            host_input(in, h->engine, h, msg.pay, msg.pay_len);
        lmb_msg_free(&msg);
        if (bad || (in->capture && in->capture->len > h->max_frame + sizeof in->header)) return -1;
        if (in->active) return 0;
    }
    return -1;
}

static void host_session_error(int fd, const char *id, const char *reason) {
    char line[256];
    int n = snprintf(line, sizeof line, "ERROR %s %s\n", id, reason);
    if (n > 0 && (size_t)n < sizeof line)
        (void)lmb_send(fd, LMB_HOST_STREAM, NULL, 0, line, (uint32_t)n);
}

static void *host_session_worker(void *opaque) {
    HostSessionSlot *slot = opaque;
    HostSessionPool *pool = slot->pool;
    HostState h = pool->host; h.routed_slot = slot->index;
    while (!g_stopping && !atomic_load(&pool->stop)) {
        LmbBuf pending = {0};
        HostInput input = {.capture=&pending}; HostOutput output = {0};
        if (host_session_receive(slot, &h, &input)) { free(pending.p); break; }
        double queued_at = nowd();
        char queued[128];
        int n = snprintf(queued, sizeof queued, "PROGRESS %s QUEUED %u %u\n",
            input.request_id, lmb_run_gate_queued(&pool->turns), pool->queue_ms);
        if (n <= 0 || (size_t)n >= sizeof queued ||
            lmb_send(slot->fd, LMB_HOST_STREAM, NULL, 0, queued, (uint32_t)n)) {
            free(pending.p); break;
        }
        int admitted = lmb_run_gate_enter(&pool->turns, pool->queue_ms, host_session_cancelled, slot);
        if (admitted != 1) {
            if (!admitted) host_session_error(slot->fd, input.request_id, "host queue deadline exceeded");
            free(pending.p); break;
        }
        if (g_stopping || atomic_load(&pool->stop)) {
            lmb_run_gate_leave(&pool->turns); free(pending.p); break;
        }
        int broken = 0;
        if (slot->reset_needed) {
            HostInput empty = {0}; HostOutput ack = {0};
            broken = host_reset_conversation(h.engine, &h, &empty, &ack);
            if (!broken) slot->reset_needed = 0;
        }
        input.capture = NULL; input.started = nowd();
        double queue_seconds = input.started - queued_at;
        if (!broken) broken = host_engine_write(h.engine, pending.p, pending.len);
        free(pending.p);
        if (!broken) broken = host_bridge(slot->fd, h.engine, &h, &input, &output);
        fprintf(stderr, "[host-session] slot=%u queue_seconds=%.6f service_seconds=%.6f completed=%d\n",
            slot->index, queue_seconds, nowd()-input.started, !broken && !input.active);
        if (!broken && !input.active) {
            lmb_run_gate_leave(&pool->turns);
            continue;
        }
        /* Disconnect/timeout cancels only the active slot. Drain its terminal
         * response before RESET_SLOT; never donate leftover bytes to another
         * conversation. A damaged shared codec stops the host explicitly. */
        if (!broken && !g_stopping && !atomic_load(&pool->stop)) {
            char cancel[96];
            n = snprintf(cancel, sizeof cancel, "CANCEL %s\n", input.request_id);
            if (n <= 0 || (size_t)n >= sizeof cancel || host_engine_write(h.engine, cancel, (size_t)n) ||
                host_reset_conversation(h.engine, &h, &input, &output)) broken = 1;
        }
        if (broken) { atomic_store(&pool->failed, 1); atomic_store(&pool->stop, 1); }
        lmb_run_gate_leave(&pool->turns);
        break;
    }
    pthread_mutex_lock(&pool->lock);
    lmb_close(slot->fd); slot->fd = -1; slot->used = 0; slot->reset_needed = 1;
    pool->workers--; pthread_cond_broadcast(&pool->empty);
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

static int host_sessions_run(int listener, const HostState *h, uint32_t queue_ms) {
    HostSessionPool pool = {.host=*h, .queue_ms=queue_ms};
    if (pthread_mutex_init(&pool.lock, NULL)) return 1;
    if (pthread_cond_init(&pool.empty, NULL)) { pthread_mutex_destroy(&pool.lock); return 1; }
    if (lmb_run_gate_init(&pool.turns, 1, h->slots-1)) {
        pthread_cond_destroy(&pool.empty); pthread_mutex_destroy(&pool.lock); return 1;
    }
    for (uint32_t i = 0; i < h->slots; i++) {
        pool.slots[i].pool = &pool; pool.slots[i].index = i; pool.slots[i].fd = -1;
    }
    int flags = fcntl(h->engine->to, F_GETFL, 0);
    if (flags < 0 || fcntl(h->engine->to, F_SETFL, flags | O_NONBLOCK) || lmb_ready_notify()) {
        lmb_run_gate_destroy(&pool.turns); pthread_cond_destroy(&pool.empty); pthread_mutex_destroy(&pool.lock);
        return 1;
    }
    while (!g_stopping && !atomic_load(&pool.stop)) {
        struct pollfd p = {listener, POLLIN, 0};
        int ready = poll(&p, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (p.revents & (POLLERR|POLLHUP|POLLNVAL))) break;
        if (!ready) continue;
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) continue;
        if (host_read_hello(fd, h)) { lmb_close(fd); continue; }
        pthread_mutex_lock(&pool.lock);
        HostSessionSlot *slot = NULL;
        for (uint32_t i = 0; i < h->slots; i++) if (!pool.slots[i].used) { slot = &pool.slots[i]; break; }
        HostState greeting = *h;
        greeting.sessions_free = h->slots-pool.workers;
        if (!slot) {
            pthread_mutex_unlock(&pool.lock);
            (void)host_greet(fd, h, 1); lmb_close(fd); continue;
        }
        slot->fd = fd; slot->used = 1; pool.workers++;
        pthread_mutex_unlock(&pool.lock);
        int greet_failed = host_greet(fd, &greeting, 0);
        pthread_mutex_lock(&pool.lock);
        pthread_t worker;
        if (greet_failed || pthread_create(&worker, NULL, host_session_worker, slot)) {
            lmb_close(fd); slot->fd = -1; slot->used = 0; pool.workers--;
        } else pthread_detach(worker);
        pthread_mutex_unlock(&pool.lock);
    }
    atomic_store(&pool.stop, 1);
    pthread_mutex_lock(&pool.lock);
    for (uint32_t i = 0; i < h->slots; i++) if (pool.slots[i].used) shutdown(pool.slots[i].fd, SHUT_RDWR);
    while (pool.workers) pthread_cond_wait(&pool.empty, &pool.lock);
    pthread_mutex_unlock(&pool.lock);
    lmb_run_gate_destroy(&pool.turns); pthread_cond_destroy(&pool.empty); pthread_mutex_destroy(&pool.lock);
    return atomic_load(&pool.failed) ? 1 : 0;
}
#endif
