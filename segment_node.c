#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lumabri_proto.h"
#include "lumabri_segment.h"
#include "lumabri_segment_discovery.h"
#include "lumabri_machine.h"
#include "lumabri_run_gate.h"
#include "lumabri_planner.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "segment_colibri.h"

#include <pthread.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define NODE_SESSIONS_MAX 256u
#define NODE_CONNECTIONS_MAX 256u
#define NODE_SNAPSHOT_CHUNK (1u << 20)

static uint64_t directory_bytes(const char *path, unsigned depth) {
    if (!path || depth > 8) return 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    uint64_t total = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[4096];
        int n = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof child) { total = UINT64_MAX; break; }
        struct stat st;
        if (lstat(child, &st)) continue;
        uint64_t add = S_ISREG(st.st_mode) ? (uint64_t)st.st_size :
                       S_ISDIR(st.st_mode) ? directory_bytes(child, depth + 1) : 0;
        if (UINT64_MAX - total < add) { total = UINT64_MAX; break; }
        total += add;
    }
    closedir(dir);
    return total;
}

typedef struct {
    int used;
    LmbSegId id;
    ColiSegmentSession *session;
    pthread_mutex_t lock;
    LmbSegId cached_request;
    uint8_t *cached_output;
    size_t cached_bytes;
    uint8_t *snapshot;
    size_t snapshot_bytes;
    uint64_t snapshot_sequence;
    uint64_t snapshot_position;
    uint8_t *restore;
    size_t restore_bytes;
    size_t restore_received;
    LmbSegTransfer restore_transfer;
    uint64_t restore_updated_ms;
    int restore_active;
    LmbSegId restored_request;
    uint64_t restored_sequence;
    uint64_t restored_position;
    int restored_valid;
} NodeSession;

typedef struct {
    pthread_mutex_t lock;
    char tracker[256];
    char local_addr[64];
    LmbSegAdvert advert;
    uint8_t pk[32], sk[64];
    LmbSegOwner owner;
    LmbRunGate *run_gate;
    int ready;
    volatile sig_atomic_t stop;
} TrackerRegistration;

typedef struct {
    ColiSegmentEngine *engine;
    ColiSegmentCapabilities cap;
    LmbSegAdvert advert;
    LmbSegTable *table;
    NodeSession sessions[NODE_SESSIONS_MAX];
    pthread_mutex_t sessions_lock;
    LmbRunGate run_gate;
    uint32_t run_wait_ms;
    pthread_mutex_t connections_lock;
    pthread_cond_t connections_drained;
    int connection_fds[NODE_CONNECTIONS_MAX];
    unsigned active_connections;
    uint64_t ram_reserve_bytes;
    uint64_t process_memory_limit_bytes;
    uint64_t session_memory_limit_bytes;
    LmbGovernor governor;
    TrackerRegistration registration;
} Node;

static volatile sig_atomic_t g_stop;
static int g_listen_fd = -1;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t available_memory_bytes(void) {
    uint64_t available = lmb_machine_available_ram();
    return available ? available : UINT64_MAX;
}

static uint64_t resident_memory_bytes(void) {
    FILE *file = fopen("/proc/self/statm", "r");
    unsigned long long pages = 0, resident = 0;
    if (!file) return 0;
    int ok = fscanf(file, "%llu %llu", &pages, &resident) == 2;
    fclose(file);
    long page = sysconf(_SC_PAGESIZE);
    if (!ok || page <= 0 || resident > UINT64_MAX / (uint64_t)page) return 0;
    return resident * (uint64_t)page;
}

static int process_budget_exhausted(Node *node) {
    uint64_t rss = resident_memory_bytes();
    return node->process_memory_limit_bytes && rss &&
           rss >= node->process_memory_limit_bytes;
}

/* OPEN and route publication stop at the reserve.  A RUN which already owns a
 * session is different: aborting it throws away the conversation state and
 * turns a harmless pressure transition into a fake peer failure.  Let active
 * work drain through PRESSURE/RECOVERY and operator pause; only shutdown or the
 * governor's critical RAM/swap floor may interrupt a Colibri kernel. */
static int run_should_cancel(void *opaque) {
    Node *node = opaque;
    return g_stop || lmb_governor_abort_inflight(&node->governor);
}

static void *governor_worker(void *opaque) {
    Node *node = opaque;
    LmbGovernorState previous = lmb_governor_state(&node->governor);
    int previous_accepting = lmb_governor_accepting(&node->governor) &&
                             !process_budget_exhausted(node);
    int first = 1;
    while (!g_stop && !node->registration.stop) {
        LmbGovernorState state = lmb_governor_poll(&node->governor);
        int process_limited = process_budget_exhausted(node);
        int accepting = state == LMB_GOV_ACTIVE && !process_limited;
        pthread_mutex_lock(&node->registration.lock);
        if (accepting)
            node->registration.advert.flags &= ~LMB_SEG_ADVERT_DRAINING;
        else
            node->registration.advert.flags |= LMB_SEG_ADVERT_DRAINING;
        pthread_mutex_unlock(&node->registration.lock);
        if (first || state != previous || accepting != previous_accepting) {
            uint64_t available = lmb_machine_available_ram();
            uint64_t rss = resident_memory_bytes();
            fprintf(stderr, "[segment-node %s] governor ",
                    node->advert.peer_name);
            if (!first) fprintf(stderr, "%s -> ",
                                lmb_governor_state_name(previous));
            fprintf(stderr, "%s · %s",
                    lmb_governor_state_name(state),
                    accepting ? "accepting sessions" :
                                "draining and refusing new work");
            if (process_limited)
                fprintf(stderr, " · reason: process memory budget reached");
            else if (state != LMB_GOV_ACTIVE)
                fprintf(stderr, " · reason: %s",
                        lmb_governor_reason_name(
                            lmb_governor_reason(&node->governor)));
            fprintf(stderr, " · available %.1f GB / reserve %.1f GB · "
                    "RSS %.1f GB / budget %.1f GB\n",
                    (double)available / 1e9,
                    (double)node->ram_reserve_bytes / 1e9,
                    (double)rss / 1e9,
                    (double)node->process_memory_limit_bytes / 1e9);
            first = 0;
            previous = state;
            previous_accepting = accepting;
        }
        for (int i = 0; i < 10 && !g_stop && !node->registration.stop; i++)
            usleep(100000);
    }
    return NULL;
}

static void stop_handler(int sig) {
    (void)sig;
    g_stop = 1;
    /* Bypass the optional secure close macro: libc close is async-signal-safe
     * and listening sockets never enter the encrypted-fd registry. */
    int fd = g_listen_fd;
    g_listen_fd = -1;
    if (fd >= 0) (close)(fd);
}

static int owner_same_lease(const LmbSegOwner *a, const LmbSegOwner *b) {
    return lmb_seg_id_equal(&a->lease_id, &b->lease_id) &&
           a->fencing_epoch == b->fencing_epoch &&
           a->route_generation >= b->route_generation;
}

static int registration_owner(TrackerRegistration *r,
                              const LmbSegOwner *candidate) {
    pthread_mutex_lock(&r->lock);
    int ok = r->ready && owner_same_lease(candidate, &r->owner);
    pthread_mutex_unlock(&r->lock);
    return ok;
}

static int tracker_register_send(int fd, const uint8_t nonce[32],
                                 TrackerRegistration *r) {
    LmbSegAdvert advert;
    pthread_mutex_lock(&r->lock);
    advert = r->advert;
    pthread_mutex_unlock(&r->lock);
    if (r->run_gate) {
        advert.queue_depth = lmb_run_gate_queued(r->run_gate);
        advert.inflight = lmb_run_gate_inflight(r->run_gate);
    }
    uint8_t *wire = NULL;
    uint32_t wire_len = 0;
    if (lmb_seg_advert_encode(&advert, &wire, &wire_len)) return -1;
    LmbBuf body = { wire, wire_len, wire_len };
    uint8_t auth[512], sig[64];
    size_t auth_len = lmb_peer_auth_msg(nonce, advert.peer_name, advert.model,
                                        advert.addr, auth, sizeof auth);
    if (!auth_len) { free(body.p); return -1; }
    lmb_sign(sig, auth, auth_len, r->sk);
    if (lmb_buf_peer_auth(&body, r->pk, sig) ||
        lmb_send(fd, LMB_SEG_REGISTER, body.p, (uint32_t)body.len, NULL, 0)) {
        free(body.p);
        return -1;
    }
    free(body.p);
    return 0;
}

static int tracker_registration_reply(TrackerRegistration *r,
                                      const LmbMsg *reply) {
    LmbSegOwner owner;
    if (reply->op != LMB_SEG_REGISTER_R || reply->pay_len ||
        lmb_seg_registration_reply_decode(reply->body, reply->body_len, &owner))
        return -1;
    pthread_mutex_lock(&r->lock);
    r->owner = owner;
    r->ready = 1;
    pthread_mutex_unlock(&r->lock);
    return 0;
}

static int segment_request_op(uint32_t op) {
    return op == LMB_SEG_OPEN || op == LMB_SEG_RUN ||
           op == LMB_SEG_SNAPSHOT || op == LMB_SEG_RESTORE ||
           op == LMB_SEG_CLOSE || op == LMB_SEG_HEALTH;
}

static int handle_rseg_fwd(TrackerRegistration *r, int tracker_fd,
                           const LmbMsg *forward) {
    LmbCur cursor = { forward->body, forward->body_len, 0 };
    uint32_t relay_id = 0, inner_op = 0, inner_body_len = 0;
    if (lmb_cur_u32(&cursor, &relay_id) ||
        lmb_cur_u32(&cursor, &inner_op) ||
        lmb_cur_u32(&cursor, &inner_body_len) ||
        !segment_request_op(inner_op) ||
        inner_body_len != cursor.len - cursor.off ||
        !lmb_frame_shape_ok(inner_op, inner_body_len, forward->pay_len))
        return -1;

    LmbMsg response = {0};
    int local_fd = lmb_connect(r->local_addr);
    int ok = local_fd >= 0 &&
             !lmb_send(local_fd, inner_op, cursor.p + cursor.off,
                       inner_body_len, forward->pay, forward->pay_len) &&
             !lmb_recv(local_fd, &response) &&
             response.op == inner_op + 1u &&
             lmb_frame_shape_ok(response.op, response.body_len,
                                response.pay_len);
    if (local_fd >= 0) lmb_close(local_fd);
    LmbBuf body = {0};
    int bad = lmb_buf_u32(&body, relay_id) ||
              lmb_buf_u32(&body, ok ? 1u : 0u) ||
              lmb_buf_u32(&body, ok ? response.op : 0u) ||
              lmb_buf_u32(&body, ok ? response.body_len : 0u) ||
              (ok && lmb_buf_bytes(&body, response.body, response.body_len));
    if (!bad)
        bad = lmb_send(tracker_fd, LMB_RSEG_R, body.p, (uint32_t)body.len,
                       ok ? response.pay : NULL,
                       ok ? response.pay_len : 0u);
    free(body.p);
    lmb_msg_free(&response);
    return bad;
}

static void *registration_worker(void *opaque) {
    TrackerRegistration *r = (TrackerRegistration *)opaque;
    while (!r->stop) {
        int fd = lmb_connect(r->tracker);
        uint8_t nonce[32];
        if (fd < 0 || lmb_auth(fd) || lmb_request_challenge(fd, nonce)) {
            if (fd >= 0) lmb_close(fd);
            sleep(1);
            continue;
        }
        /* Poll provides the short heartbeat cadence; once a frame starts,
         * allow a slow WAN relay enough time to receive the complete bounded
         * activation/snapshot instead of treating a 200 ms partial read as a
         * broken control tunnel. */
        struct timeval timeout = {60, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        uint64_t last_heartbeat = now_ms();
        if (tracker_register_send(fd, nonce, r)) {
            lmb_close(fd); sleep(1); continue;
        }
        while (!r->stop) {
            struct pollfd input = { .fd = fd, .events = POLLIN };
            int ready = poll(&input, 1, 200);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (!ready) {
                if (now_ms() - last_heartbeat >= 2000u) {
                    if (tracker_register_send(fd, nonce, r)) break;
                    last_heartbeat = now_ms();
                }
                continue;
            }
            if (input.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            LmbMsg message = {0};
            if (lmb_recv(fd, &message)) {
                break;
            }
            int bad = 0;
            if (message.op == LMB_SEG_REGISTER_R)
                bad = tracker_registration_reply(r, &message);
            else if (message.op == LMB_RSEG_FWD)
                bad = handle_rseg_fwd(r, fd, &message);
            else if (message.op == LMB_ERR) {
                fprintf(stderr,
                        "[segment-node] tracker rejected control message\n");
                bad = 1;
            }
            lmb_msg_free(&message);
            if (bad) break;
        }
        lmb_close(fd);
        pthread_mutex_lock(&r->lock);
        r->ready = 0;
        pthread_mutex_unlock(&r->lock);
    }
    return NULL;
}

static int open_contract_matches_node(Node *node, const LmbSegOpen *open) {
    const LmbSegAdvert *a = &node->advert;
    return !memcmp(open->model_root, a->model_root, sizeof a->model_root) &&
           !memcmp(open->tokenizer_root, a->tokenizer_root,
                   sizeof a->tokenizer_root) &&
           open->layer_begin == a->layer_begin &&
           open->layer_end == a->layer_end &&
           open->context_tokens <= a->max_context &&
           open->max_rows <= a->max_rows &&
           open->state_dtype == a->state_dtype &&
           open->state_width == a->state_width &&
           (open->capabilities & a->capabilities) == open->capabilities &&
           !strcmp(open->engine_id, a->engine_id) &&
           !strcmp(open->state_schema, a->state_schema) &&
           !strcmp(open->numeric_class, a->numeric_class);
}

static NodeSession *session_find(Node *node, const LmbSegId *id) {
    for (size_t i = 0; i < NODE_SESSIONS_MAX; i++)
        if (node->sessions[i].used && lmb_seg_id_equal(&node->sessions[i].id, id))
            return &node->sessions[i];
    return NULL;
}

static NodeSession *session_empty(Node *node) {
    for (size_t i = 0; i < NODE_SESSIONS_MAX; i++)
        if (!node->sessions[i].used) return &node->sessions[i];
    return NULL;
}

/* sessions_lock and slot->lock are both held. Keeping the slow wait for the
 * slot out of sessions_lock is what lets unrelated OPEN/CLOSE calls proceed
 * while a multi-GB restore is committing on one session. */
static void session_release_locked(Node *node, NodeSession *slot) {
    coli_segment_session_destroy(slot->session);
    free(slot->cached_output);
    free(slot->snapshot);
    free(slot->restore);
    slot->used = 0;
    memset(&slot->id, 0, sizeof slot->id);
    slot->session = NULL;
    memset(&slot->cached_request, 0, sizeof slot->cached_request);
    slot->cached_output = NULL;
    slot->cached_bytes = 0;
    slot->snapshot = NULL;
    slot->snapshot_bytes = 0;
    slot->snapshot_sequence = slot->snapshot_position = 0;
    slot->restore = NULL;
    slot->restore_bytes = slot->restore_received = 0;
    memset(&slot->restore_transfer, 0, sizeof slot->restore_transfer);
    slot->restore_updated_ms = 0;
    slot->restore_active = 0;
    memset(&slot->restored_request, 0, sizeof slot->restored_request);
    slot->restored_sequence = slot->restored_position = 0;
    slot->restored_valid = 0;
    pthread_mutex_unlock(&slot->lock);
    pthread_mutex_lock(&node->registration.lock);
    if (node->registration.advert.active_sessions)
        node->registration.advert.active_sessions--;
    pthread_mutex_unlock(&node->registration.lock);
}

static void session_release(Node *node, NodeSession *slot) {
    pthread_mutex_lock(&slot->lock);
    session_release_locked(node, slot);
}

/* Slot mutexes live for the whole node lifetime. Find under the table lock,
 * then wait outside it and validate after taking the slot: a restore on one
 * session can never stop OPEN/CLOSE/RUN on an unrelated session. */
static NodeSession *session_lock_id(Node *node, const LmbSegId *id) {
    pthread_mutex_lock(&node->sessions_lock);
    NodeSession *slot = session_find(node, id);
    pthread_mutex_unlock(&node->sessions_lock);
    if (!slot) return NULL;
    pthread_mutex_lock(&slot->lock);
    if (!slot->used || !lmb_seg_id_equal(&slot->id, id)) {
        pthread_mutex_unlock(&slot->lock);
        return NULL;
    }
    return slot;
}

static void *session_reaper(void *opaque) {
    Node *node = (Node *)opaque;
    while (!g_stop) {
        pthread_mutex_lock(&node->sessions_lock);
        for (size_t i = 0; i < NODE_SESSIONS_MAX; i++) {
            NodeSession *slot = &node->sessions[i];
            if (!slot->used) continue;
            /* Never wait for a restore while holding the node-wide table
             * lock. The next pass will inspect a busy slot. */
            if (pthread_mutex_trylock(&slot->lock)) continue;
            if (slot->restore_active &&
                now_ms() - slot->restore_updated_ms >= 60000u) {
                (void)lmb_seg_table_restore_finish(
                    node->table, &slot->restore_transfer, 0, now_ms());
                free(slot->restore); slot->restore = NULL;
                slot->restore_bytes = slot->restore_received = 0;
                slot->restore_active = 0;
            }
            pthread_mutex_unlock(&slot->lock);
        }
        pthread_mutex_unlock(&node->sessions_lock);
        LmbSegId expired;
        while (lmb_seg_table_reap_expired(node->table, now_ms(), &expired)) {
            NodeSession *slot = session_lock_id(node, &expired);
            if (!slot) continue;
            pthread_mutex_lock(&node->sessions_lock);
            if (slot->used && lmb_seg_id_equal(&slot->id, &expired))
                session_release_locked(node, slot);
            else
                pthread_mutex_unlock(&slot->lock);
            pthread_mutex_unlock(&node->sessions_lock);
        }
        for (int i = 0; i < 10 && !g_stop; i++) usleep(100000);
    }
    return NULL;
}

static int connection_add(Node *node, int fd) {
    int added = 0;
    pthread_mutex_lock(&node->connections_lock);
    for (size_t i = 0; i < NODE_CONNECTIONS_MAX; i++) {
        if (node->connection_fds[i] >= 0) continue;
        node->connection_fds[i] = fd;
        node->active_connections++;
        added = 1;
        break;
    }
    pthread_mutex_unlock(&node->connections_lock);
    return added ? 0 : -1;
}

static void connection_remove(Node *node, int fd) {
    pthread_mutex_lock(&node->connections_lock);
    for (size_t i = 0; i < NODE_CONNECTIONS_MAX; i++) {
        if (node->connection_fds[i] != fd) continue;
        node->connection_fds[i] = -1;
        if (node->active_connections) node->active_connections--;
        break;
    }
    if (!node->active_connections) pthread_cond_signal(&node->connections_drained);
    pthread_mutex_unlock(&node->connections_lock);
}

static LmbSegReply make_reply(const LmbSegId *session_id,
                              const LmbSegId *request_id,
                              const LmbSegOwner *owner,
                              LmbSegStatus status) {
    LmbSegReply reply;
    memset(&reply, 0, sizeof reply);
    if (session_id) reply.session_id = *session_id;
    if (request_id) reply.request_id = *request_id;
    reply.status = (uint32_t)status;
    if (owner) {
        reply.fencing_epoch = owner->fencing_epoch;
        reply.route_generation = owner->route_generation;
    }
    return reply;
}

static void reply_state(Node *node, LmbSegReply *reply) {
    int needs_restore = 0;
    (void)lmb_seg_table_state(node->table, &reply->session_id,
                              &reply->next_sequence, &reply->next_position,
                              &needs_restore);
}

static int send_reply(int fd, uint32_t op, LmbSegReply *reply,
                      const void *pay, size_t pay_len) {
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (pay_len > UINT32_MAX || lmb_seg_reply_encode(reply, &body, &body_len))
        return -1;
    int rc = lmb_send(fd, op, body, body_len, pay, (uint32_t)pay_len);
    free(body);
    return rc;
}

static int send_transfer_reply(int fd, uint32_t op,
                               LmbSegTransferReply *reply,
                               const void *pay, size_t pay_len) {
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (pay_len > UINT32_MAX ||
        lmb_seg_transfer_reply_encode(reply, &body, &body_len)) return -1;
    int rc = lmb_send(fd, op, body, body_len, pay, (uint32_t)pay_len);
    free(body);
    return rc;
}

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t limit;
} SnapshotBuffer;

static int snapshot_write(void *opaque, const void *data, size_t size) {
    SnapshotBuffer *buffer = opaque;
    if (size > buffer->limit - buffer->length) return -1;
    size_t needed = buffer->length + size;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096u;
        while (capacity < needed) {
            if (capacity > buffer->limit / 2u) { capacity = buffer->limit; break; }
            capacity *= 2u;
        }
        uint8_t *grown = realloc(buffer->data, capacity);
        if (!grown) return -1;
        buffer->data = grown; buffer->capacity = capacity;
    }
    if (size) memcpy(buffer->data + buffer->length, data, size);
    buffer->length = needed;
    return 0;
}

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
} SnapshotReader;

static int snapshot_read(void *opaque, void *data, size_t size) {
    SnapshotReader *reader = opaque;
    if (size > reader->length - reader->offset) return -1;
    if (size) memcpy(data, reader->data + reader->offset, size);
    reader->offset += size;
    return 0;
}

static int handle_open(Node *node, int fd, const LmbMsg *msg) {
    LmbSegOpen open;
    if (msg->pay_len || lmb_seg_open_decode(msg->body, msg->body_len, &open))
        return -1;
    LmbSegStatus status = LMB_SEG_STATUS_BAD_REQUEST;
    int contract_matches = open_contract_matches_node(node, &open);
    if (contract_matches &&
        !registration_owner(&node->registration, &open.owner)) {
        status = LMB_SEG_STATUS_STALE_OWNER;
    } else if (contract_matches) {
        pthread_mutex_lock(&node->sessions_lock);
        NodeSession *slot = session_find(node, &open.session_id);
        if (slot) {
            status = lmb_seg_table_open(node->table, &open, now_ms());
        } else if (!(slot = session_empty(node))) {
            status = LMB_SEG_STATUS_QUOTA;
        } else if (!lmb_governor_accepting(&node->governor) ||
                   available_memory_bytes() < node->ram_reserve_bytes ||
                   process_budget_exhausted(node)) {
            status = LMB_SEG_STATUS_QUOTA;
        } else {
            ColiSegmentSessionOptions options = {
                .struct_size = sizeof options,
                .context_tokens = open.context_tokens,
                .memory_limit_bytes = node->session_memory_limit_bytes,
            };
            ColiSegmentSession *session = NULL;
            char error[256] = "";
            if (coli_segment_session_create(node->engine, &options, &session,
                                            error, sizeof error)) {
                fprintf(stderr, "[segment-node] session create: %s\n",
                        engine_error(error));
                status = LMB_SEG_STATUS_INTERNAL;
            } else {
                status = lmb_seg_table_open(node->table, &open, now_ms());
                if (status == LMB_SEG_STATUS_OK) {
                    pthread_mutex_lock(&slot->lock);
                    slot->used = 1;
                    slot->id = open.session_id;
                    slot->session = session;
                    pthread_mutex_lock(&node->registration.lock);
                    node->registration.advert.active_sessions++;
                    pthread_mutex_unlock(&node->registration.lock);
                    pthread_mutex_unlock(&slot->lock);
                } else {
                    coli_segment_session_destroy(session);
                }
            }
        }
        pthread_mutex_unlock(&node->sessions_lock);
    }
    LmbSegReply reply = make_reply(&open.session_id, &open.request_id,
                                   &open.owner, status);
    if (status == LMB_SEG_STATUS_OK || status == LMB_SEG_STATUS_DUPLICATE)
        reply_state(node, &reply);
    return send_reply(fd, LMB_SEG_OPEN_R, &reply, NULL, 0);
}

static int handle_run(Node *node, int fd, const LmbMsg *msg) {
    int32_t *tokens = calloc(node->cap.max_batch_rows, sizeof *tokens);
    if (!tokens) return -1;
    LmbSegRun run;
    if (lmb_seg_run_decode(msg->body, msg->body_len, &run, tokens,
                           node->cap.max_batch_rows)) {
        free(tokens);
        return -1;
    }
    LmbSegStatus status = lmb_seg_table_run_begin(
        node->table, &run, msg->pay, msg->pay_len, now_ms());
    LmbSegReply reply = make_reply(&run.session_id, &run.request_id,
                                   &run.owner, status);
    if (status == LMB_SEG_STATUS_DUPLICATE) {
        NodeSession *slot = session_lock_id(node, &run.session_id);
        if (slot && lmb_seg_id_equal(&slot->cached_request, &run.request_id) &&
            slot->cached_output) {
            reply_state(node, &reply);
            int rc = send_reply(fd, LMB_SEG_RUN_R, &reply,
                                slot->cached_output, slot->cached_bytes);
            pthread_mutex_unlock(&slot->lock);
            free(tokens);
            return rc;
        }
        if (slot) pthread_mutex_unlock(&slot->lock);
        reply.status = LMB_SEG_STATUS_NEEDS_RESTORE;
    } else if (status == LMB_SEG_STATUS_OK) {
        NodeSession *slot = session_lock_id(node, &run.session_id);
        ColiSegmentSession *session = slot ? slot->session : NULL;
        size_t bytes = lmb_state_bytes(run.rows, node->cap.state_width,
                                       node->cap.state_dtype);
        uint8_t *output = bytes ? malloc(bytes) : NULL;
        if (!session || !output || bytes != msg->pay_len) {
            status = LMB_SEG_STATUS_INTERNAL;
        } else {
            int admitted = lmb_run_gate_enter(&node->run_gate,
                                               node->run_wait_ms,
                                               run_should_cancel, node);
            if (admitted != 1) {
                status = admitted < 0 ? LMB_SEG_STATUS_QUOTA :
                                        LMB_SEG_STATUS_BUSY;
                if (admitted < 0)
                    fprintf(stderr, "[segment-node %s %u:%u] RUN admission "
                                    "cancelled: %s%s (available %.1f GB / "
                                    "reserve %.1f GB)\n",
                            node->advert.peer_name, node->advert.layer_begin,
                            node->advert.layer_end,
                            g_stop ? "process stopping" :
                                lmb_governor_reason_name(
                                    lmb_governor_reason(&node->governor)),
                            g_stop ? "" : " reached the in-flight safety floor",
                            (double)available_memory_bytes() / 1e9,
                            (double)node->ram_reserve_bytes / 1e9);
            }
            ColiSegmentRunRequest request = {
                .struct_size = sizeof request,
                .rows = run.rows,
                .position = run.position,
                .token_ids = run.token_ids,
                .token_count = run.token_count,
                .input = msg->pay,
                .input_bytes = msg->pay_len,
                .output = output,
                .output_bytes = bytes,
                .should_cancel = run_should_cancel,
                .cancel_user_data = node,
            };
            char error[256] = "";
            if (admitted == 1 &&
                coli_segment_run(session, &request, error, sizeof error)) {
                /* Name the slice: an origin runs several of these at once and
                 * an unlabelled line cannot be attributed to a layer range. */
                fprintf(stderr, "[segment-node %s %u:%u] run failed on %u row%s "
                        "at position %llu: %s\n",
                        node->advert.peer_name, node->advert.layer_begin,
                        node->advert.layer_end, run.rows,
                        run.rows == 1 ? "" : "s",
                        (unsigned long long)run.position,
                        engine_error(error));
                if (lmb_governor_abort_inflight(&node->governor)) {
                    fprintf(stderr, "[segment-node %s] in-flight emergency: %s "
                                    "(available %.1f GB / reserve %.1f GB)\n",
                            node->advert.peer_name,
                            lmb_governor_reason_name(
                                lmb_governor_reason(&node->governor)),
                            (double)available_memory_bytes() / 1e9,
                            (double)node->ram_reserve_bytes / 1e9);
                    status = LMB_SEG_STATUS_QUOTA;
                } else {
                    status = LMB_SEG_STATUS_INTERNAL;
                }
            }
            if (admitted == 1) lmb_run_gate_leave(&node->run_gate);
        }
        if (status != LMB_SEG_STATUS_OK) {
            (void)lmb_seg_table_run_abort(node->table, &run);
            free(output);
            if (slot) pthread_mutex_unlock(&slot->lock);
        } else {
            status = lmb_seg_table_run_commit(node->table, &run, now_ms());
            if (status == LMB_SEG_STATUS_OK && slot) {
                free(slot->cached_output);
                slot->cached_output = output;
                slot->cached_bytes = bytes;
                slot->cached_request = run.request_id;
                free(slot->snapshot);
                slot->snapshot = NULL;
                slot->snapshot_bytes = 0;
                reply.status = status;
                reply_state(node, &reply);
                int rc = send_reply(fd, LMB_SEG_RUN_R, &reply, output, bytes);
                pthread_mutex_unlock(&slot->lock);
                free(tokens);
                return rc;
            }
            if (slot) pthread_mutex_unlock(&slot->lock);
            free(output);
        }
        reply.status = status;
    }
    reply_state(node, &reply);
    free(tokens);
    return send_reply(fd, LMB_SEG_RUN_R, &reply, NULL, 0);
}

static int handle_snapshot(Node *node, int fd, const LmbMsg *msg) {
    LmbSegTransfer transfer;
    if (msg->pay_len ||
        lmb_seg_transfer_decode(msg->body, msg->body_len, &transfer)) return -1;
    LmbSegStatus status = lmb_seg_table_snapshot_check(
        node->table, &transfer, now_ms());
    LmbSegTransferReply response;
    memset(&response, 0, sizeof response);
    response.reply = make_reply(&transfer.session_id, &transfer.request_id,
                                &transfer.owner, status);
    uint8_t *payload = NULL;
    size_t payload_bytes = 0;
    if (status == LMB_SEG_STATUS_OK) {
        NodeSession *slot = session_lock_id(node, &transfer.session_id);
        if (!slot) status = LMB_SEG_STATUS_NOT_FOUND;
        else {
            int initial = transfer.snapshot_size == 0 && transfer.offset == 0 &&
                          transfer.chunk_len == 0;
            if (initial) {
                SnapshotBuffer writer = {
                    .limit = (size_t)lmb_env_int(
                        "LUMABRI_SEGMENT_MAX_SNAPSHOT_MB", 2048, 1, 32768) << 20,
                };
                char error[256] = {0};
                if (coli_segment_snapshot(slot->session, snapshot_write, &writer,
                                          error, sizeof error) ||
                    !writer.length) {
                    fprintf(stderr, "[segment-node] snapshot: %s\n",
                            error[0] ? error : "empty or oversized snapshot");
                    free(writer.data);
                    status = LMB_SEG_STATUS_INTERNAL;
                } else {
                    free(slot->snapshot);
                    slot->snapshot = writer.data;
                    slot->snapshot_bytes = writer.length;
                    slot->snapshot_sequence = transfer.sequence;
                    slot->snapshot_position = transfer.position;
                }
            } else if (!slot->snapshot ||
                       transfer.snapshot_size != slot->snapshot_bytes ||
                       transfer.sequence != slot->snapshot_sequence ||
                       transfer.position != slot->snapshot_position) {
                status = LMB_SEG_STATUS_CONFLICT;
            }
            if (status == LMB_SEG_STATUS_OK) {
                uint64_t offset = initial ? 0 : transfer.offset;
                size_t requested = initial ? NODE_SNAPSHOT_CHUNK
                                           : transfer.chunk_len;
                if (!requested || offset > slot->snapshot_bytes) {
                    status = LMB_SEG_STATUS_BAD_REQUEST;
                } else {
                    size_t remaining = slot->snapshot_bytes - (size_t)offset;
                    payload_bytes = remaining < requested ? remaining : requested;
                    payload = malloc(payload_bytes ? payload_bytes : 1u);
                    if (!payload) {
                        status = LMB_SEG_STATUS_INTERNAL;
                        payload_bytes = 0;
                    } else if (payload_bytes) {
                        memcpy(payload, slot->snapshot + offset, payload_bytes);
                    }
                    response.snapshot_size = slot->snapshot_bytes;
                    response.offset = offset;
                    response.chunk_len = (uint32_t)payload_bytes;
                    if (!offset) response.transfer_flags |= LMB_SEG_XFER_BEGIN;
                    if (payload_bytes == remaining)
                        response.transfer_flags |= LMB_SEG_XFER_END;
                }
            }
            pthread_mutex_unlock(&slot->lock);
        }
    }
    response.reply.status = status;
    if (status == LMB_SEG_STATUS_OK) reply_state(node, &response.reply);
    else {
        response.snapshot_size = response.offset = 0;
        response.chunk_len = response.transfer_flags = 0;
        free(payload); payload = NULL; payload_bytes = 0;
    }
    int rc = send_transfer_reply(fd, LMB_SEG_SNAPSHOT_R, &response,
                                 payload, payload_bytes);
    free(payload);
    return rc;
}

static int handle_restore(Node *node, int fd, const LmbMsg *msg) {
    LmbSegTransfer transfer;
    if (lmb_seg_transfer_decode(msg->body, msg->body_len, &transfer) ||
        msg->pay_len != transfer.chunk_len) return -1;
    LmbSegStatus status = LMB_SEG_STATUS_OK;
    NodeSession *slot = session_lock_id(node, &transfer.session_id);
    if (!slot) status = LMB_SEG_STATUS_NOT_FOUND;
    else if (slot->restored_valid &&
             lmb_seg_id_equal(&slot->restored_request, &transfer.request_id) &&
             slot->restored_sequence == transfer.sequence &&
             slot->restored_position == transfer.position) {
        status = LMB_SEG_STATUS_DUPLICATE;
    } else {
        int same = slot->restore_active &&
                   lmb_seg_id_equal(&slot->restore_transfer.request_id,
                                    &transfer.request_id) &&
                   slot->restore_transfer.snapshot_size == transfer.snapshot_size &&
                   slot->restore_transfer.sequence == transfer.sequence &&
                   slot->restore_transfer.position == transfer.position;
        if (transfer.flags & LMB_SEG_XFER_BEGIN) {
            if (!same) {
                status = lmb_seg_table_restore_begin(
                    node->table, &transfer, now_ms());
                size_t limit = (size_t)lmb_env_int(
                    "LUMABRI_SEGMENT_MAX_SNAPSHOT_MB", 2048, 1, 32768) << 20;
                if (status == LMB_SEG_STATUS_OK &&
                    transfer.snapshot_size > limit)
                    status = LMB_SEG_STATUS_QUOTA;
                if (status == LMB_SEG_STATUS_OK) {
                    free(slot->restore);
                    slot->restore = malloc((size_t)transfer.snapshot_size);
                    if (!slot->restore) status = LMB_SEG_STATUS_INTERNAL;
                }
                if (status == LMB_SEG_STATUS_OK) {
                    slot->restore_bytes = (size_t)transfer.snapshot_size;
                    slot->restore_received = 0;
                    slot->restore_transfer = transfer;
                    slot->restore_active = 1;
                } else if (status != LMB_SEG_STATUS_BUSY) {
                    (void)lmb_seg_table_restore_finish(
                        node->table, &transfer, 0, now_ms());
                }
            }
        } else if (!same) {
            status = LMB_SEG_STATUS_CONFLICT;
        }
        if (status == LMB_SEG_STATUS_OK && slot->restore_active) {
            if (transfer.offset < slot->restore_received &&
                transfer.chunk_len <= slot->restore_received - transfer.offset &&
                !memcmp(slot->restore + transfer.offset,
                        msg->pay, transfer.chunk_len)) {
                status = LMB_SEG_STATUS_DUPLICATE;
            } else if (transfer.offset != slot->restore_received ||
                       transfer.chunk_len >
                           slot->restore_bytes - slot->restore_received) {
                status = LMB_SEG_STATUS_OUT_OF_ORDER;
            } else {
                if (transfer.chunk_len)
                    memcpy(slot->restore + slot->restore_received,
                           msg->pay, transfer.chunk_len);
                slot->restore_received += transfer.chunk_len;
                slot->restore_updated_ms = now_ms();
                if (transfer.flags & LMB_SEG_XFER_END) {
                    SnapshotReader reader = {
                        slot->restore, slot->restore_bytes, 0
                    };
                    char error[256] = {0};
                    int restored = slot->restore_received == slot->restore_bytes &&
                        !coli_segment_restore(slot->session, snapshot_read,
                                              &reader, error, sizeof error) &&
                        reader.offset == reader.length;
                    if (!restored)
                        fprintf(stderr, "[segment-node] restore: %s\n",
                                error[0] ? error : "truncated snapshot");
                    status = lmb_seg_table_restore_finish(
                        node->table, &transfer, restored, now_ms());
                    if (!restored && status == LMB_SEG_STATUS_OK)
                        status = LMB_SEG_STATUS_INTERNAL;
                    if (restored && status == LMB_SEG_STATUS_OK) {
                        slot->restored_request = transfer.request_id;
                        slot->restored_sequence = transfer.sequence;
                        slot->restored_position = transfer.position;
                        slot->restored_valid = 1;
                        free(slot->cached_output);
                        slot->cached_output = NULL; slot->cached_bytes = 0;
                    }
                    free(slot->restore); slot->restore = NULL;
                    slot->restore_bytes = slot->restore_received = 0;
                    slot->restore_active = 0;
                }
            }
        }
    }
    if (slot) pthread_mutex_unlock(&slot->lock);
    LmbSegReply reply = make_reply(&transfer.session_id, &transfer.request_id,
                                   &transfer.owner, status);
    if (status == LMB_SEG_STATUS_OK || status == LMB_SEG_STATUS_DUPLICATE)
        reply_state(node, &reply);
    return send_reply(fd, LMB_SEG_RESTORE_R, &reply, NULL, 0);
}

static int handle_control(Node *node, int fd, const LmbMsg *msg) {
    LmbSegControl control;
    if (msg->pay_len ||
        lmb_seg_control_decode(msg->body, msg->body_len, &control)) return -1;
    LmbSegStatus status;
    uint32_t response_op;
    if (msg->op == LMB_SEG_CLOSE) {
        response_op = LMB_SEG_CLOSE_R;
        pthread_mutex_lock(&node->sessions_lock);
        NodeSession *slot = session_find(node, &control.session_id);
        if (slot && pthread_mutex_trylock(&slot->lock)) {
            /* The client retries CLOSE; every other session remains free to
             * open or close while this one finishes restore/run. */
            status = LMB_SEG_STATUS_BUSY;
        } else {
            status = lmb_seg_table_close(node->table, &control);
            if (status == LMB_SEG_STATUS_OK && slot)
                session_release_locked(node, slot);
            else if (slot)
                pthread_mutex_unlock(&slot->lock);
        }
        pthread_mutex_unlock(&node->sessions_lock);
    } else {
        uint64_t next_sequence = 0, next_position = 0;
        int needs_restore = 0;
        status = lmb_seg_table_state(node->table, &control.session_id,
                                     &next_sequence, &next_position,
                                     &needs_restore);
        response_op = LMB_SEG_HEALTH_R;
    }
    LmbSegReply reply = make_reply(&control.session_id, &control.request_id,
                                   &control.owner, status);
    if (status == LMB_SEG_STATUS_OK && msg->op != LMB_SEG_CLOSE)
        reply_state(node, &reply);
    return send_reply(fd, response_op, &reply, NULL, 0);
}

typedef struct { Node *node; int fd; } Connection;

static void *connection_worker(void *opaque) {
    Connection *connection = (Connection *)opaque;
    Node *node = connection->node;
    int fd = connection->fd;
    free(connection);
    for (;;) {
        LmbMsg msg = {0};
        if (lmb_recv(fd, &msg)) break;
        int rc;
        if (msg.op == LMB_PING) {
            lmb_emu_delay();
            rc = lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
        }
        else if (msg.op == LMB_SEG_OPEN) rc = handle_open(node, fd, &msg);
        else if (msg.op == LMB_SEG_RUN) rc = handle_run(node, fd, &msg);
        else if (msg.op == LMB_SEG_SNAPSHOT) rc = handle_snapshot(node, fd, &msg);
        else if (msg.op == LMB_SEG_RESTORE) rc = handle_restore(node, fd, &msg);
        else if (msg.op == LMB_SEG_CLOSE || msg.op == LMB_SEG_HEALTH)
            rc = handle_control(node, fd, &msg);
        else rc = lmb_send(fd, LMB_ERR, "unsupported Segment operation", 29,
                           NULL, 0);
        lmb_msg_free(&msg);
        if (rc) break;
    }
    lmb_close(fd);
    connection_remove(node, fd);
    return NULL;
}

static void usage(const char *program) {
    fprintf(stderr,
        "usage: %s --engine ID --model-dir DIR --model NAME --range A:B "
        "(--range A:B | --auto-range) --port N --tracker HOST:PORT "
        "--advertise HOST:PORT --name PEER "
        "(--auto-identity | --model-root HEX64 --tokenizer-root HEX64) "
        "[--fallback] [--relay-only] [--context N] [--max-rows N] "
        "[--sessions N] [--threads N] [--run-queue N] [--run-wait-ms N] "
        "[--memory-limit-mb N] "
        "[--model-bytes N --model-layers N] [--preflight-fd N]\n",
        program);
}

static const char *arg_value(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static int has_arg(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], name)) return 1;
    return 0;
}

static int auto_range_get(const char *tracker, const char *model,
                          const char *name, const char *engine_id,
                          const uint8_t model_root[LMB_SEG_ROOT_BYTES],
                          unsigned *begin, unsigned *end,
                          unsigned *model_layers) {
    LmbBuf body = {0};
    if (lmb_buf_u32(&body, LMB_SEG_ASSIGN_MAGIC) ||
        lmb_buf_u32(&body, LMB_SEG_ASSIGN_VERSION) ||
        lmb_buf_str(&body, model) || lmb_buf_str(&body, name) ||
        lmb_buf_str(&body, engine_id) ||
        lmb_buf_bytes(&body, model_root, LMB_SEG_ROOT_BYTES)) {
        free(body.p); return -1;
    }
    LmbMsg reply = {0};
    int rc = lmb_request(tracker, LMB_SEG_ASSIGN, body.p,
                         (uint32_t)body.len, &reply);
    free(body.p);
    if (rc || reply.op != LMB_SEG_ASSIGN_R || reply.pay_len) {
        lmb_msg_free(&reply); return -1;
    }
    LmbCur cursor = { reply.body, reply.body_len, 0 };
    uint32_t magic = 0, version = 0, first = 0, last = 0, total = 0;
    rc = lmb_cur_u32(&cursor, &magic) ||
         lmb_cur_u32(&cursor, &version) ||
         lmb_cur_u32(&cursor, &first) || lmb_cur_u32(&cursor, &last) ||
         lmb_cur_u32(&cursor, &total) ||
         cursor.off != cursor.len || magic != LMB_SEG_ASSIGN_MAGIC ||
         version != LMB_SEG_ASSIGN_VERSION || first >= last || last > total;
    lmb_msg_free(&reply);
    if (rc) return -1;
    *begin = first; *end = last; *model_layers = total;
    return 0;
}

static int parse_u64_decimal(const char *text, uint64_t *out) {
    if (!text || !*text || text[0] == '-') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || end == text || *end) return -1;
    *out = (uint64_t)value;
    return 0;
}

static int auto_range_release(const char *tracker, const char *model,
                              const char *name, const char *engine_id,
                              const uint8_t model_root[LMB_SEG_ROOT_BYTES]) {
    LmbBuf body = {0};
    if (lmb_buf_u32(&body, LMB_SEG_ASSIGN_MAGIC) ||
        lmb_buf_u32(&body, LMB_SEG_ASSIGN_VERSION) ||
        lmb_buf_str(&body, model) || lmb_buf_str(&body, name) ||
        lmb_buf_str(&body, engine_id) ||
        lmb_buf_bytes(&body, model_root, LMB_SEG_ROOT_BYTES)) {
        free(body.p); return -1;
    }
    LmbMsg reply = {0};
    int rc = lmb_request(tracker, LMB_SEG_ASSIGN_RELEASE, body.p,
                         (uint32_t)body.len, &reply);
    free(body.p);
    int bad = rc || reply.op != LMB_OK || reply.body_len || reply.pay_len;
    lmb_msg_free(&reply);
    return bad ? -1 : 0;
}

static void preflight_signal(int *fd, char status) {
    if (!fd || *fd < 0) return;
    while (write(*fd, &status, 1) < 0 && errno == EINTR) { }
    close(*fd);
    *fd = -1;
}

int main(int argc, char **argv) {
    const char *engine_id = arg_value(argc, argv, "--engine");
    const char *model_dir = arg_value(argc, argv, "--model-dir");
    const char *model = arg_value(argc, argv, "--model");
    const char *range = arg_value(argc, argv, "--range");
    const char *port_text = arg_value(argc, argv, "--port");
    const char *tracker = arg_value(argc, argv, "--tracker");
    const char *advertise = arg_value(argc, argv, "--advertise");
    const char *name = arg_value(argc, argv, "--name");
    const char *model_root = arg_value(argc, argv, "--model-root");
    const char *tokenizer_root = arg_value(argc, argv, "--tokenizer-root");
    int auto_identity = has_arg(argc, argv, "--auto-identity");
    int auto_range = has_arg(argc, argv, "--auto-range");
    int fallback = has_arg(argc, argv, "--fallback");
    int relay_only = has_arg(argc, argv, "--relay-only");
    if (!engine_id || !model_dir || !model || ((range != NULL) == auto_range) ||
        !port_text || !tracker ||
        !advertise || !name ||
        (auto_identity ? (model_root || tokenizer_root)
                       : (!model_root || !tokenizer_root))) {
        usage(argv[0]); return 2;
    }
    unsigned begin = 0, end = 0;
    char trailing;
    uint32_t port;
    if (strlen(engine_id) >= LMB_SEG_ENGINE_MAX ||
        strlen(model) >= LMB_SEG_MODEL_MAX ||
        strlen(tracker) >= sizeof ((TrackerRegistration *)0)->tracker ||
        strlen(advertise) >= LMB_SEG_ADDR_MAX ||
        strlen(name) >= LMB_SEG_PEER_NAME_MAX ||
        (range && (sscanf(range, "%u:%u%c", &begin, &end, &trailing) != 2 ||
                   begin >= end)) ||
        lmb_parse_u32(port_text, 1, 65535, &port)) {
        usage(argv[0]); return 2;
    }
    uint32_t context = 4096, max_rows = 256, max_sessions = 16, threads = 0;
    uint32_t run_queue = 32, run_wait_ms = 30000;
    uint32_t memory_limit_mb = 0, model_layers = 0, preflight_fd_u32 = 0;
    uint64_t model_bytes = 0;
    int preflight_fd = -1;
    const char *value;
    if (((value = arg_value(argc, argv, "--context")) &&
         lmb_parse_u32(value, 1, LMB_SEG_MAX_CONTEXT, &context)) ||
        ((value = arg_value(argc, argv, "--max-rows")) &&
         lmb_parse_u32(value, 1, LMB_SEG_MAX_ROWS, &max_rows)) ||
        ((value = arg_value(argc, argv, "--sessions")) &&
         lmb_parse_u32(value, 1, NODE_SESSIONS_MAX, &max_sessions)) ||
        ((value = arg_value(argc, argv, "--threads")) &&
         lmb_parse_u32(value, 1, 256, &threads)) ||
        ((value = arg_value(argc, argv, "--run-queue")) &&
         lmb_parse_u32(value, 0, NODE_SESSIONS_MAX, &run_queue)) ||
        ((value = arg_value(argc, argv, "--run-wait-ms")) &&
         lmb_parse_u32(value, 50, 60000, &run_wait_ms)) ||
        ((value = arg_value(argc, argv, "--memory-limit-mb")) &&
         lmb_parse_u32(value, 1, 1048576, &memory_limit_mb)) ||
        ((value = arg_value(argc, argv, "--model-bytes")) &&
         parse_u64_decimal(value, &model_bytes)) ||
        ((value = arg_value(argc, argv, "--model-layers")) &&
         lmb_parse_u32(value, 1, 1048576, &model_layers)) ||
        ((value = arg_value(argc, argv, "--preflight-fd")) &&
         lmb_parse_u32(value, 0, 1048576, &preflight_fd_u32)) ||
        (range && model_bytes && !model_layers)) {
        usage(argv[0]); return 2;
    }
    if (arg_value(argc, argv, "--preflight-fd"))
        preflight_fd = (int)preflight_fd_u32;
#ifdef _OPENMP
    if (threads) omp_set_num_threads((int)threads);
#else
    if (threads > 1) {
        fprintf(stderr, "--threads needs an OpenMP build\n"); return 2;
    }
#endif
    if (lmb_secure_init()) return 1;
    signal(SIGINT, stop_handler); signal(SIGTERM, stop_handler);
    signal(SIGPIPE, SIG_IGN);
    /* The hybrid engine is a Lumabri-only build copy. Its transport client
     * discovers resident Expert peers from this node's own control-plane
     * identity; Colibri sources and ordinary Colibri binaries stay untouched. */
    setenv("LUMABRI_TRACKER", tracker, 1);
    setenv("LUMABRI_MODEL", model, 1);
    setenv("LUMABRI_EXEC_FALLBACK_LOCAL", "1", 1);
    /* The GLM Segment adapter is named "glm" while its long-standing Expert
     * wire family is "colibri". Every other adapter shares the same name.
     * Bits describe expert storage, not the activation/state representation. */
    const char *expert_engine_id = !strcmp(engine_id, "glm") ?
                                   "colibri" : engine_id;
    setenv("LUMABRI_ENGINE_ID", expert_engine_id, 1);
    setenv("LUMABRI_EXPERT_BITS",
           (!strcmp(expert_engine_id, "colibri") ||
            !strcmp(expert_engine_id, "inkling")) ? "8" : "0", 1);
    if (fallback) (void)setpriority(PRIO_PROCESS, 0, 10);
    if (threads)
        fprintf(stderr, "[segment-node] governor: %u compute thread%s · "
                        "%u session%s max%s\n", threads,
                threads == 1 ? "" : "s", max_sessions,
                max_sessions == 1 ? "" : "s",
                (fallback || auto_range) ? " · low CPU priority" : "");
    /* The upstream batched expert union currently rejects the V4 Flash
     * checkpoint used by Segment. Disable it before adapter registration in
     * this dedicated process: ordinary Colibri and expert_node keep their
     * own environment and their normal code path. Retrying after a failed
     * RUN is unsafe because recurrent/attention state may already have moved. */
    if (!strcmp(engine_id, "deepseek_v4") && !getenv("V4_EXPERT_UNION")) {
        setenv("V4_EXPERT_UNION", "0", 0);
        fprintf(stderr, "[segment-node] DeepSeek V4 batched expert union "
                        "disabled for Segment safety\n");
    }
    if (lmb_colibri_register_all()) {
        fprintf(stderr, "cannot register all six Colibri adapters\n"); return 1;
    }
    uint8_t resolved_model_root[LMB_SEG_ROOT_BYTES];
    uint8_t resolved_tokenizer_root[LMB_SEG_ROOT_BYTES];
    if (auto_identity) {
        LmbModelIdentity identity;
        int announced = 0;
        while (!g_stop && lmb_model_identity_get(tracker, model, &identity)) {
            if (!announced) {
                fprintf(stderr, "[segment-node] waiting for model identity "
                        "%s from tracker %s\n", model, tracker);
                announced = 1;
            }
            struct timespec pause = {1, 0};
            while (!g_stop && nanosleep(&pause, &pause) && errno == EINTR) { }
        }
        if (g_stop) return 0;
        const char *pubkey = getenv("LUMABRI_PUBKEY");
        if (pubkey && pubkey[0]) {
            LmbTrustKeys trust = {0};
            size_t bytes = 0;
            uint8_t *message = lmb_model_id_msg(identity.model, identity.root,
                                                &bytes);
            int bad = lmb_trust_load_spec(&trust, pubkey) || !identity.has_sig ||
                      !message || lmb_trust_verify(&trust, identity.sig,
                                                   message, bytes);
            free(message);
            if (bad) {
                fprintf(stderr, "[segment-node] untrusted model identity\n");
                return 1;
            }
        }
        memcpy(resolved_model_root, identity.root,
               sizeof resolved_model_root);
        /* tokenizer.json is a member of the aggregate signed inventory. */
        memcpy(resolved_tokenizer_root, identity.root,
               sizeof resolved_tokenizer_root);
    } else if (lmb_hex_root(model_root, resolved_model_root) ||
               lmb_hex_root(tokenizer_root, resolved_tokenizer_root)) {
        fprintf(stderr, "roots must be non-zero 64-character hex values\n");
        return 2;
    }

    if (auto_range) {
        int announced = 0;
        while (!g_stop && auto_range_get(tracker, model, name, engine_id,
                                         resolved_model_root, &begin, &end,
                                         &model_layers)) {
            if (!announced) {
                fprintf(stderr, "[segment-node] waiting for an automatic "
                        "range assignment for %s\n", model);
                announced = 1;
            }
            struct timespec pause = {1, 0};
            while (!g_stop && nanosleep(&pause, &pause) && errno == EINTR) { }
        }
        if (g_stop) return 0;
        fprintf(stderr, "[segment-node] tracker assigned layers %u:%u\n",
                begin, end);
        /* A desktop compute donation must lose scheduling contests to the
         * user's own applications. Server origins use --fallback, which
         * applies the same priority; explicit low-level nodes remain neutral. */
        (void)setpriority(PRIO_PROCESS, 0, 10);
    }
    uint64_t process_limit = (uint64_t)memory_limit_mb << 20;
    uint64_t available = available_memory_bytes();
    const char *reserve_name = getenv("LUMABRI_SEGMENT_RAM_RESERVE_MB") ?
                               "LUMABRI_SEGMENT_RAM_RESERVE_MB" :
                               "LUMABRI_RAM_RESERVE_MB";
    uint64_t reserve = (uint64_t)lmb_env_int(
        reserve_name, 4096, 256, 262144) << 20;
    if (!process_limit && available != UINT64_MAX && available > reserve)
        process_limit = available - reserve;
    /* The runtime preflight must be conservative. The generic catalogue
     * arithmetic is not an adapter contract and cannot authorize a smaller
     * allocation; in particular setting a "disk" environment flag did not
     * make any Colibri adapter stream its weights. Until an adapter exposes a
     * verified minimum-working-set callback, retain the proven guard: its
     * proportional share of the actual checkpoint plus overhead. */
    LmbModelShape shape;
    int shaped = model_dir && !lmb_shape_from_config(model_dir, &shape);
    if (!model_layers && shaped) model_layers = shape.layers;
    if (!model_bytes && model_dir) model_bytes = directory_bytes(model_dir, 0);
    if (process_limit && model_bytes && model_layers) {
        uint64_t range_layers = end - begin;
        uint64_t proportional = model_bytes / model_layers * range_layers;
        uint64_t remainder = model_bytes % model_layers * range_layers /
                             model_layers;
        uint64_t overhead = model_bytes / 20u;
        uint64_t estimated = proportional + remainder;
        if (estimated <= UINT64_MAX - overhead) estimated += overhead;
        else estimated = UINT64_MAX;
        if (estimated > process_limit) {
            fprintf(stderr, "[segment-node] assigned range %u:%u needs about "
                    "%.1f GB but the donor budget is %.1f GB; releasing it "
                    "before loading weights (disk mode is not verified for "
                    "this adapter)\n", begin, end,
                    (double)estimated / 1e9, (double)process_limit / 1e9);
            if (auto_range)
                (void)auto_range_release(tracker, model, name, engine_id,
                                         resolved_model_root);
            preflight_signal(&preflight_fd, 'F');
            return 3;
        }
    }
    preflight_signal(&preflight_fd, 'P');
    Node node;
    memset(&node, 0, sizeof node);
    for (size_t i = 0; i < NODE_CONNECTIONS_MAX; i++) node.connection_fds[i] = -1;
    pthread_mutex_init(&node.sessions_lock, NULL);
    if (lmb_run_gate_init(&node.run_gate, 1, run_queue)) {
        fprintf(stderr, "cannot initialize Segment admission gate\n");
        return 1;
    }
    node.run_wait_ms = run_wait_ms;
    pthread_mutex_init(&node.connections_lock, NULL);
    pthread_cond_init(&node.connections_drained, NULL);
    for (size_t i = 0; i < NODE_SESSIONS_MAX; i++)
        pthread_mutex_init(&node.sessions[i].lock, NULL);
    node.ram_reserve_bytes = reserve;
    lmb_governor_init(&node.governor, reserve);
    node.process_memory_limit_bytes = process_limit;
    fprintf(stderr, "[segment-node] governor: %.1f GB RAM reserved for the "
                    "machine\n", (double)node.ram_reserve_bytes / 1e9);
    fprintf(stderr, "[segment-node] admission: one engine team · FIFO queue "
                    "%u · %u ms deadline\n", run_queue, run_wait_ms);
    ColiSegmentEngineOptions options = {
        .struct_size = sizeof options,
        .model_dir = model_dir,
        .layer_begin = begin,
        .layer_end = end,
        .context_tokens = context,
        .memory_limit_bytes = process_limit,
    };
    char error[256] = "";
    if (coli_segment_engine_open(engine_id, &options, &node.engine,
                                 error, sizeof error)) {
        fprintf(stderr, "cannot open Colibri Segment engine: %s\n",
                engine_error(error));
        if (auto_range)
            (void)auto_range_release(tracker, model, name, engine_id,
                                     resolved_model_root);
        return 1;
    }
    node.cap.struct_size = sizeof node.cap;
    error[0] = 0;
    if (coli_segment_engine_capabilities(node.engine, &node.cap,
                                         error, sizeof error)) {
        fprintf(stderr, "cannot read Segment capabilities: %s\n",
                engine_error(error));
        if (auto_range)
            (void)auto_range_release(tracker, model, name, engine_id,
                                     resolved_model_root);
        return 1;
    }
    uint64_t engine_rss = resident_memory_bytes();
    if (process_limit && engine_rss && engine_rss >= process_limit) {
        fprintf(stderr, "[segment-node] engine RSS %.1f GB exhausted the "
                        "%.1f GB process budget before opening sessions\n",
                (double)engine_rss / 1e9, (double)process_limit / 1e9);
        if (auto_range)
            (void)auto_range_release(tracker, model, name, engine_id,
                                     resolved_model_root);
        return 1;
    }
    if (process_limit && process_limit > engine_rss)
        node.session_memory_limit_bytes =
            (process_limit - engine_rss) / max_sessions;
    fprintf(stderr, "[segment-node] governor: %.1f GB process budget · "
                    "%.1f MB per session\n",
            (double)process_limit / 1e9,
            (double)node.session_memory_limit_bytes / 1e6);
    if (end > node.cap.num_layers || context > node.cap.max_context_tokens ||
        max_rows > node.cap.max_batch_rows) {
        fprintf(stderr, "requested range/context/rows exceed model capabilities\n");
        if (auto_range)
            (void)auto_range_release(tracker, model, name, engine_id,
                                     resolved_model_root);
        return 1;
    }
    LmbSegAdvert *a = &node.advert;
    snprintf(a->peer_name, sizeof a->peer_name, "%s", name);
    snprintf(a->addr, sizeof a->addr, "%s", advertise);
    snprintf(a->model, sizeof a->model, "%s", model);
    memcpy(a->model_root, resolved_model_root, sizeof a->model_root);
    memcpy(a->tokenizer_root, resolved_tokenizer_root,
           sizeof a->tokenizer_root);
    a->layer_begin = begin; a->layer_end = end;
    a->max_context = context; a->max_rows = max_rows;
    a->state_dtype = node.cap.state_dtype; a->state_width = node.cap.state_width;
    a->max_sessions = max_sessions;
    if (fallback) a->flags |= LMB_SEG_ADVERT_FALLBACK;
    if (relay_only) a->flags |= LMB_SEG_ADVERT_RELAY_ONLY;
    if (!lmb_governor_accepting(&node.governor))
        a->flags |= LMB_SEG_ADVERT_DRAINING;
    a->capabilities = node.cap.flags & LMB_SEG_CAP_KNOWN_MASK;
    /* Registration happens after engine_open, so this is measured resident
     * memory, not a model-size promise. A zero value remains valid on systems
     * without an RSS counter, but the node is still READY only after open. */
    a->resident_ram_bytes = engine_rss;
    snprintf(a->engine_id, sizeof a->engine_id, "%s", node.cap.engine_id);
    snprintf(a->state_schema, sizeof a->state_schema, "%s", node.cap.state_schema);
    snprintf(a->numeric_class, sizeof a->numeric_class, "%s", node.cap.numeric_class);
    if (!lmb_seg_advert_valid(a)) {
        fprintf(stderr, "generated Segment advert is invalid\n"); return 1;
    }
    node.table = lmb_seg_table_create(max_sessions);
    if (!node.table) { fprintf(stderr, "cannot create session table\n"); return 1; }
    TrackerRegistration *registration = &node.registration;
    registration->run_gate = &node.run_gate;
    pthread_mutex_init(&registration->lock, NULL);
    snprintf(registration->tracker, sizeof registration->tracker, "%s", tracker);
    snprintf(registration->local_addr, sizeof registration->local_addr,
             "127.0.0.1:%u", port);
    registration->advert = *a;
    char peer_key_path[512];
    if (lmb_peer_identity(lmb_peer_key_path(peer_key_path,
                                            sizeof peer_key_path),
                          registration->sk, registration->pk)) {
        fprintf(stderr, "cannot load persistent peer identity from %s\n",
                peer_key_path);
        return 1;
    }
    g_listen_fd = lmb_listen((int)port);
    if (g_listen_fd < 0) { perror("segment listen"); return 1; }
    pthread_t registration_thread, reaper_thread, governor_thread;
    if (pthread_create(&registration_thread, NULL, registration_worker,
                       registration)) {
        fprintf(stderr, "cannot start tracker registration\n"); return 1;
    }
    if (pthread_create(&reaper_thread, NULL, session_reaper, &node)) {
        fprintf(stderr, "cannot start session reaper\n");
        registration->stop = 1;
        pthread_join(registration_thread, NULL);
        return 1;
    }
    if (pthread_create(&governor_thread, NULL, governor_worker, &node)) {
        fprintf(stderr, "cannot start resource governor\n");
        g_stop = 1; registration->stop = 1;
        pthread_join(reaper_thread, NULL);
        pthread_join(registration_thread, NULL);
        return 1;
    }
    printf("[segment-node] %s %s layers %u:%u at %s (tracker %s)\n",
           engine_id, model, begin, end, advertise, tracker);
    fflush(stdout);
    while (!g_stop) {
        int fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0) { if (g_stop) break; continue; }
        if (lmb_secure_server(fd)) { lmb_close(fd); continue; }
        lmb_set_io_timeout(fd, lmb_env_int("LUMABRI_IO_TIMEOUT_MS",
                           LMB_DEFAULT_IO_TIMEOUT_MS, 100, 3600000));
        Connection *connection = malloc(sizeof *connection);
        if (!connection) { close(fd); continue; }
        connection->node = &node; connection->fd = fd;
        if (connection_add(&node, fd)) {
            free(connection); lmb_close(fd); continue;
        }
        pthread_t thread;
        if (pthread_create(&thread, NULL, connection_worker, connection)) {
            connection_remove(&node, fd);
            free(connection); lmb_close(fd); continue;
        }
        pthread_detach(thread);
    }
    registration->stop = 1;
    pthread_join(governor_thread, NULL);
    pthread_join(reaper_thread, NULL);
    pthread_join(registration_thread, NULL);
    pthread_mutex_lock(&node.connections_lock);
    for (size_t i = 0; i < NODE_CONNECTIONS_MAX; i++)
        if (node.connection_fds[i] >= 0)
            shutdown(node.connection_fds[i], SHUT_RDWR);
    while (node.active_connections)
        pthread_cond_wait(&node.connections_drained, &node.connections_lock);
    pthread_mutex_unlock(&node.connections_lock);
    pthread_mutex_lock(&node.sessions_lock);
    for (size_t i = 0; i < NODE_SESSIONS_MAX; i++) {
        NodeSession *slot = &node.sessions[i];
        if (!slot->used) continue;
        session_release(&node, slot);
    }
    pthread_mutex_unlock(&node.sessions_lock);
    lmb_seg_table_destroy(node.table);
    error[0] = 0;
    if (coli_segment_engine_close(node.engine, error, sizeof error))
        fprintf(stderr, "[segment-node] engine close: %s\n",
                engine_error(error));
    memset(registration->sk, 0, sizeof registration->sk);
    pthread_cond_destroy(&node.connections_drained);
    pthread_mutex_destroy(&node.connections_lock);
    pthread_mutex_destroy(&node.sessions_lock);
    lmb_run_gate_destroy(&node.run_gate);
    for (size_t i = 0; i < NODE_SESSIONS_MAX; i++)
        pthread_mutex_destroy(&node.sessions[i].lock);
    pthread_mutex_destroy(&registration->lock);
    return 0;
}
