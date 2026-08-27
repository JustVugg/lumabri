#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lumabri_proto.h"
#include "lumabri_segment.h"
#include "lumabri_segment_discovery.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "segment_colibri.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define NODE_SESSIONS_MAX 256u
#define NODE_CONNECTIONS_MAX 256u

typedef struct {
    int used;
    LmbSegId id;
    ColiSegmentSession *session;
    pthread_mutex_t lock;
    LmbSegId cached_request;
    uint8_t *cached_output;
    size_t cached_bytes;
} NodeSession;

typedef struct {
    pthread_mutex_t lock;
    char tracker[256];
    LmbSegAdvert advert;
    uint8_t pk[32], sk[64];
    LmbSegOwner owner;
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
    pthread_mutex_t connections_lock;
    pthread_cond_t connections_drained;
    int connection_fds[NODE_CONNECTIONS_MAX];
    unsigned active_connections;
    TrackerRegistration registration;
} Node;

static volatile sig_atomic_t g_stop;
static int g_listen_fd = -1;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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

static int tracker_heartbeat(int fd, const uint8_t nonce[32],
                             TrackerRegistration *r) {
    LmbSegAdvert advert;
    pthread_mutex_lock(&r->lock);
    advert = r->advert;
    pthread_mutex_unlock(&r->lock);
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
    LmbMsg reply = {0};
    LmbSegOwner owner;
    int received = !lmb_recv(fd, &reply);
    if (received && reply.op == LMB_ERR)
        fprintf(stderr, "[segment-node] tracker rejected registration: %.*s\n",
                (int)reply.body_len, reply.body ? (char *)reply.body : "");
    int bad = !received || reply.op != LMB_SEG_REGISTER_R || reply.pay_len ||
              lmb_seg_registration_reply_decode(reply.body, reply.body_len,
                                                 &owner);
    lmb_msg_free(&reply);
    if (bad) return -1;
    pthread_mutex_lock(&r->lock);
    r->owner = owner;
    r->ready = 1;
    pthread_mutex_unlock(&r->lock);
    return 0;
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
        while (!r->stop && !tracker_heartbeat(fd, nonce, r)) {
            for (int i = 0; i < 20 && !r->stop; i++) usleep(100000);
        }
        lmb_close(fd);
        pthread_mutex_lock(&r->lock);
        r->ready = 0;
        pthread_mutex_unlock(&r->lock);
    }
    return NULL;
}

static int open_matches_node(Node *node, const LmbSegOpen *open) {
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
           !strcmp(open->numeric_class, a->numeric_class) &&
           registration_owner(&node->registration, &open->owner);
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

static void session_release(Node *node, NodeSession *slot) {
    pthread_mutex_lock(&slot->lock);
    coli_segment_session_destroy(slot->session);
    free(slot->cached_output);
    pthread_mutex_unlock(&slot->lock);
    pthread_mutex_destroy(&slot->lock);
    memset(slot, 0, sizeof *slot);
    pthread_mutex_lock(&node->registration.lock);
    if (node->registration.advert.active_sessions)
        node->registration.advert.active_sessions--;
    pthread_mutex_unlock(&node->registration.lock);
}

static void *session_reaper(void *opaque) {
    Node *node = (Node *)opaque;
    while (!g_stop) {
        pthread_mutex_lock(&node->sessions_lock);
        LmbSegId expired;
        while (lmb_seg_table_reap_expired(node->table, now_ms(), &expired)) {
            NodeSession *slot = session_find(node, &expired);
            if (slot) session_release(node, slot);
        }
        pthread_mutex_unlock(&node->sessions_lock);
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

static int handle_open(Node *node, int fd, const LmbMsg *msg) {
    LmbSegOpen open;
    if (msg->pay_len || lmb_seg_open_decode(msg->body, msg->body_len, &open))
        return -1;
    LmbSegStatus status = LMB_SEG_STATUS_BAD_REQUEST;
    if (open_matches_node(node, &open)) {
        pthread_mutex_lock(&node->sessions_lock);
        NodeSession *slot = session_find(node, &open.session_id);
        if (slot) {
            status = lmb_seg_table_open(node->table, &open, now_ms());
        } else if (!(slot = session_empty(node))) {
            status = LMB_SEG_STATUS_QUOTA;
        } else {
            ColiSegmentSessionOptions options = {
                .struct_size = sizeof options,
                .context_tokens = open.context_tokens,
            };
            ColiSegmentSession *session = NULL;
            char error[256];
            if (coli_segment_session_create(node->engine, &options, &session,
                                            error, sizeof error)) {
                fprintf(stderr, "[segment-node] session create: %s\n", error);
                status = LMB_SEG_STATUS_INTERNAL;
            } else {
                status = lmb_seg_table_open(node->table, &open, now_ms());
                if (status == LMB_SEG_STATUS_OK) {
                    memset(slot, 0, sizeof *slot);
                    if (pthread_mutex_init(&slot->lock, NULL)) {
                        (void)lmb_seg_table_close(node->table,
                            &(LmbSegControl){ .session_id = open.session_id,
                                .request_id = open.request_id,
                                .owner = open.owner });
                        coli_segment_session_destroy(session);
                        status = LMB_SEG_STATUS_INTERNAL;
                        pthread_mutex_unlock(&node->sessions_lock);
                        LmbSegReply reply = make_reply(
                            &open.session_id, &open.request_id,
                            &open.owner, status);
                        return send_reply(fd, LMB_SEG_OPEN_R, &reply, NULL, 0);
                    }
                    slot->used = 1;
                    slot->id = open.session_id;
                    slot->session = session;
                    pthread_mutex_lock(&node->registration.lock);
                    node->registration.advert.active_sessions++;
                    pthread_mutex_unlock(&node->registration.lock);
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
        pthread_mutex_lock(&node->sessions_lock);
        NodeSession *slot = session_find(node, &run.session_id);
        if (slot) pthread_mutex_lock(&slot->lock);
        pthread_mutex_unlock(&node->sessions_lock);
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
        pthread_mutex_lock(&node->sessions_lock);
        NodeSession *slot = session_find(node, &run.session_id);
        ColiSegmentSession *session = slot ? slot->session : NULL;
        pthread_mutex_unlock(&node->sessions_lock);
        size_t bytes = lmb_state_bytes(run.rows, node->cap.state_width,
                                       node->cap.state_dtype);
        uint8_t *output = bytes ? malloc(bytes) : NULL;
        if (!session || !output || bytes != msg->pay_len) {
            status = LMB_SEG_STATUS_INTERNAL;
        } else {
            pthread_mutex_lock(&node->registration.lock);
            node->registration.advert.inflight++;
            pthread_mutex_unlock(&node->registration.lock);
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
            };
            char error[256];
            if (coli_segment_run(session, &request, error, sizeof error)) {
                fprintf(stderr, "[segment-node] run: %s\n", error);
                status = LMB_SEG_STATUS_INTERNAL;
            }
            pthread_mutex_lock(&node->registration.lock);
            if (node->registration.advert.inflight)
                node->registration.advert.inflight--;
            pthread_mutex_unlock(&node->registration.lock);
        }
        if (status != LMB_SEG_STATUS_OK) {
            (void)lmb_seg_table_run_abort(node->table, &run);
            free(output);
        } else {
            pthread_mutex_lock(&node->sessions_lock);
            slot = session_find(node, &run.session_id);
            if (slot) pthread_mutex_lock(&slot->lock);
            pthread_mutex_unlock(&node->sessions_lock);
            status = lmb_seg_table_run_commit(node->table, &run, now_ms());
            if (status == LMB_SEG_STATUS_OK && slot) {
                free(slot->cached_output);
                slot->cached_output = output;
                slot->cached_bytes = bytes;
                slot->cached_request = run.request_id;
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

static int handle_control(Node *node, int fd, const LmbMsg *msg) {
    LmbSegControl control;
    if (msg->pay_len ||
        lmb_seg_control_decode(msg->body, msg->body_len, &control)) return -1;
    LmbSegStatus status;
    uint32_t response_op;
    if (msg->op == LMB_SEG_CLOSE) {
        status = lmb_seg_table_close(node->table, &control);
        response_op = LMB_SEG_CLOSE_R;
        if (status == LMB_SEG_STATUS_OK) {
            pthread_mutex_lock(&node->sessions_lock);
            NodeSession *slot = session_find(node, &control.session_id);
            if (slot) session_release(node, slot);
            pthread_mutex_unlock(&node->sessions_lock);
        }
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
        if (msg.op == LMB_SEG_OPEN) rc = handle_open(node, fd, &msg);
        else if (msg.op == LMB_SEG_RUN) rc = handle_run(node, fd, &msg);
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
        "[--fallback] [--context N] [--max-rows N] [--sessions N]\n",
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
                          unsigned *begin, unsigned *end) {
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
    uint32_t magic = 0, version = 0, first = 0, last = 0;
    rc = lmb_cur_u32(&cursor, &magic) ||
         lmb_cur_u32(&cursor, &version) ||
         lmb_cur_u32(&cursor, &first) || lmb_cur_u32(&cursor, &last) ||
         cursor.off != cursor.len || magic != LMB_SEG_ASSIGN_MAGIC ||
         version != LMB_SEG_ASSIGN_VERSION || first >= last;
    lmb_msg_free(&reply);
    if (rc) return -1;
    *begin = first; *end = last;
    return 0;
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
    uint32_t context = 4096, max_rows = 256, max_sessions = 16;
    const char *value;
    if (((value = arg_value(argc, argv, "--context")) &&
         lmb_parse_u32(value, 1, LMB_SEG_MAX_CONTEXT, &context)) ||
        ((value = arg_value(argc, argv, "--max-rows")) &&
         lmb_parse_u32(value, 1, LMB_SEG_MAX_ROWS, &max_rows)) ||
        ((value = arg_value(argc, argv, "--sessions")) &&
         lmb_parse_u32(value, 1, NODE_SESSIONS_MAX, &max_sessions))) {
        usage(argv[0]); return 2;
    }
    if (lmb_secure_init()) return 1;
    signal(SIGINT, stop_handler); signal(SIGTERM, stop_handler);
    signal(SIGPIPE, SIG_IGN);
    if (fallback) (void)setpriority(PRIO_PROCESS, 0, 10);
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
                                         resolved_model_root, &begin, &end)) {
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
    Node node;
    memset(&node, 0, sizeof node);
    for (size_t i = 0; i < NODE_CONNECTIONS_MAX; i++) node.connection_fds[i] = -1;
    pthread_mutex_init(&node.sessions_lock, NULL);
    pthread_mutex_init(&node.connections_lock, NULL);
    pthread_cond_init(&node.connections_drained, NULL);
    ColiSegmentEngineOptions options = {
        .struct_size = sizeof options,
        .model_dir = model_dir,
        .layer_begin = begin,
        .layer_end = end,
        .context_tokens = context,
    };
    char error[256];
    if (coli_segment_engine_open(engine_id, &options, &node.engine,
                                 error, sizeof error)) {
        fprintf(stderr, "cannot open Colibri Segment engine: %s\n", error);
        return 1;
    }
    node.cap.struct_size = sizeof node.cap;
    if (coli_segment_engine_capabilities(node.engine, &node.cap,
                                         error, sizeof error)) {
        fprintf(stderr, "cannot read Segment capabilities: %s\n", error);
        return 1;
    }
    if (end > node.cap.num_layers || context > node.cap.max_context_tokens ||
        max_rows > node.cap.max_batch_rows) {
        fprintf(stderr, "requested range/context/rows exceed model capabilities\n");
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
    a->capabilities = node.cap.flags & LMB_SEG_CAP_KNOWN_MASK;
    /* Snapshot/replay is intentionally not on the network data plane yet. */
    a->capabilities &= ~LMB_SEG_CAP_SNAPSHOT;
    snprintf(a->engine_id, sizeof a->engine_id, "%s", node.cap.engine_id);
    snprintf(a->state_schema, sizeof a->state_schema, "%s", node.cap.state_schema);
    snprintf(a->numeric_class, sizeof a->numeric_class, "%s", node.cap.numeric_class);
    if (!lmb_seg_advert_valid(a)) {
        fprintf(stderr, "generated Segment advert is invalid\n"); return 1;
    }
    node.table = lmb_seg_table_create(max_sessions);
    if (!node.table) { fprintf(stderr, "cannot create session table\n"); return 1; }
    TrackerRegistration *registration = &node.registration;
    pthread_mutex_init(&registration->lock, NULL);
    snprintf(registration->tracker, sizeof registration->tracker, "%s", tracker);
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
    pthread_t registration_thread, reaper_thread;
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
    if (coli_segment_engine_close(node.engine, error, sizeof error))
        fprintf(stderr, "[segment-node] engine close: %s\n", error);
    memset(registration->sk, 0, sizeof registration->sk);
    pthread_cond_destroy(&node.connections_drained);
    pthread_mutex_destroy(&node.connections_lock);
    pthread_mutex_destroy(&node.sessions_lock);
    pthread_mutex_destroy(&registration->lock);
    return 0;
}
