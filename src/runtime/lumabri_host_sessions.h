/* Shared Edge weights, isolated codec slots, and bounded FIFO turn admission.
 * Colibri itself remains single-threaded at Edge: only the turn-gate owner may
 * touch its pipes. A client never chooses the internal slot or reads another
 * client's output. The host owns sockets until their worker has left. */
#ifndef LMB_HOST_SESSIONS_H
#define LMB_HOST_SESSIONS_H

typedef struct HostSessionPool HostSessionPool;
typedef struct {
    HostSessionPool *pool;
    int fd, used, reset_needed, admitted;
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
    LmbHostControl control;
    _Atomic int stop, failed;
};

static int host_session_draining(HostSessionPool *pool) {
    pthread_mutex_lock(&pool->lock);
    int draining=(int)pool->control.draining;
    pthread_mutex_unlock(&pool->lock);
    return draining;
}

static void host_session_control(int fd, HostSessionPool *pool, const LmbMsg *request) {
    const HostState *h=&pool->host;
    uint8_t root[32], expected_root[32], instance[32]; uint32_t version,action; uint64_t expected;
    LmbCur c={request->body,request->body_len,0};
    /* A household token alone is not allocation ownership. Require both the
     * pinned original requester and the exact approved checkpoint root. */
    if (!h->client_key || !h->model_root || strlen(h->model_root)!=64 ||
        !lmb_secure_peer_matches(fd,h->client_key) || lmb_unhex(expected_root,h->model_root,32) ||
        request->pay_len || request->body_len!=LMB_HOST_CONTROL_REQUEST_BYTES ||
        lmb_cur_u32(&c,&version) || version!=LMB_HOST_CONTROL_VERSION ||
        lmb_cur_u32(&c,&action) || action>LMB_HOST_CONTROL_RETIRE ||
        lmb_cur_bytes(&c,root,32) || memcmp(root,expected_root,32) ||
        lmb_cur_bytes(&c,instance,32) || lmb_cur_u64(&c,&expected) || c.off!=c.len) return;
    pthread_mutex_lock(&pool->lock);
    pool->control.connections=pool->workers;
    uint32_t status=lmb_host_control_apply(&pool->control,action,instance,expected) ? LMB_HOST_CONTROL_CONFLICT : 0;
    LmbHostControl snapshot=pool->control; snapshot.connections=pool->workers;
    pthread_mutex_unlock(&pool->lock);
    /* The acceptor serializes controls. RETIRE seals admission irreversibly
     * only with no workers, so no other writer owns the codec. Acknowledge it
     * after every retained slot has confirmed RESET/CLOSE. A failed reset
     * makes the host unavailable, never a positive retirement receipt. */
    if (!status && action==LMB_HOST_CONTROL_RETIRE) {
        for (uint32_t i=0;i<h->slots;i++) if (pool->slots[i].reset_needed) {
            HostState reset=*h; reset.routed_slot=i;
            HostInput in={0}; HostOutput out={0};
            if (host_reset_conversation(h->engine,&reset,&in,&out)) {
                atomic_store(&pool->failed,1); atomic_store(&pool->stop,1); return;
            }
            pool->slots[i].reset_needed=0;
        }
    }
    if (action) {
        char instance_text[65]; lmb_hex(instance_text,snapshot.instance,32);
        fprintf(stderr,"[host-control] action=%s instance=%s revision=%llu conflict=%u connections=%u admitted=%u\n",
            action==LMB_HOST_CONTROL_RETIRE ? "retire" : action==LMB_HOST_CONTROL_DRAIN ? "drain" : "resume",instance_text,
            (unsigned long long)snapshot.revision,status,snapshot.connections,snapshot.requests);
    }
    LmbBuf body={0};
    if (!lmb_host_control_reply(&body,status,root,&snapshot))
        (void)lmb_send(fd,LMB_HOST_CONTROL_R,body.p,(uint32_t)body.len,NULL,0);
    free(body.p);
}

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
        if (host_session_draining(slot->pool)) return -1;
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
        pthread_mutex_lock(&pool->lock);
        int draining=(int)pool->control.draining;
        if (!draining) { slot->admitted=1; pool->control.requests++; }
        pthread_mutex_unlock(&pool->lock);
        if (draining) {
            host_session_error(slot->fd,input.request_id,"host draining; request not admitted");
            free(pending.p); break;
        }
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
            pthread_mutex_lock(&pool->lock);
            slot->admitted=0; pool->control.requests--;
            pthread_mutex_unlock(&pool->lock);
            continue;
        }
        /* Disconnect/timeout cancels only the active slot. Drain its terminal
         * response before RESET_SLOT; never donate leftover bytes to another
         * conversation. A damaged shared codec stops the host explicitly. */
        if (!broken && !g_stopping && !atomic_load(&pool->stop)) {
            char cancel[96];
            n = snprintf(cancel, sizeof cancel, "CANCEL %s\n", input.request_id);
            if ((h.engine->session_slots && (n <= 0 || (size_t)n >= sizeof cancel ||
                 host_engine_write(h.engine, cancel, (size_t)n))) ||
                host_reset_conversation(h.engine, &h, &input, &output)) broken = 1;
        }
        if (broken) { atomic_store(&pool->failed, 1); atomic_store(&pool->stop, 1); }
        lmb_run_gate_leave(&pool->turns);
        break;
    }
    int reset_needed=1;
    /* Preserve the original single-conversation lifetime: closing a client
     * retires its KV now, not when another client happens to arrive. This
     * slot remains occupied, so no other worker can write the one-slot engine.
     * Multi-slot cleanup still resets under the shared turn gate on reuse. */
    if (h.slots==1 && !g_stopping && !atomic_load(&pool->stop)) {
        HostInput empty={0}; HostOutput ack={0};
        if (!host_reset_conversation(h.engine,&h,&empty,&ack)) {
            reset_needed=0;
            fprintf(stderr,"[host] conversation reset; resident weights retained\n");
        } else if (!g_stopping && !atomic_load(&pool->stop)) {
            atomic_store(&pool->failed,1); atomic_store(&pool->stop,1);
        }
    }
    pthread_mutex_lock(&pool->lock);
    if (slot->admitted) { slot->admitted=0; pool->control.requests--; }
    lmb_close(slot->fd); slot->fd = -1; slot->used = 0; slot->reset_needed = reset_needed;
    pool->workers--; pthread_cond_broadcast(&pool->empty);
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

static int host_sessions_run(int listener, const HostState *h, uint32_t queue_ms) {
    HostSessionPool pool = {.host=*h, .queue_ms=queue_ms};
    pool.host.pooled=1;
    pool.control.revision=1; lmb_random(pool.control.instance,sizeof pool.control.instance);
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
        LmbMsg request={0};
        if (host_read_request(fd,h,&request)) { lmb_close(fd); continue; }
        if (request.op==LMB_HOST_CONTROL) {
            host_session_control(fd,&pool,&request); lmb_msg_free(&request); lmb_close(fd); continue;
        }
        int invalid=request.op!=LMB_HOST_HELLO || request.body_len || request.pay_len;
        lmb_msg_free(&request);
        if (invalid) { lmb_close(fd); continue; }
        pthread_mutex_lock(&pool.lock);
        HostSessionSlot *slot = NULL;
        for (uint32_t i = 0; i < h->slots; i++) if (!pool.slots[i].used) { slot = &pool.slots[i]; break; }
        HostState greeting = *h;
        greeting.sessions_free = h->slots-pool.workers;
        if (!slot || pool.control.draining) {
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
