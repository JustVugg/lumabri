#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lumabri_proto.h"
#include "lumabri_segment.h"
#include "lumabri_segment_discovery.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "lumabri_sampling.h"
#include "segment_colibri.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    LmbSegRouteEntry route;
    int fd;
    int direct_failed;
    int opened;
    char transport_error[256];
    LmbSegOpen open;
    uint64_t sequence;
    uint64_t position;
    uint8_t *snapshot;
    size_t snapshot_bytes;
    uint64_t snapshot_sequence;
    uint64_t snapshot_position;
} RemoteSegment;

typedef struct {
    int active;
    uint64_t route_generation;
    RemoteSegment chain[LMB_SEG_ROUTE_MAX];
    size_t chain_count;
    /* Tokens whose state is already committed on every remote range. The
     * final sampled token is deliberately absent until the next turn sends
     * it through the chain. */
    int32_t *committed_tokens;
    size_t committed_count;
    size_t checkpoint_count;
} SegmentConversation;

typedef struct {
    char *text;
    size_t text_bytes;
    int32_t *tokens;
    size_t token_count;
    size_t prompt_count;
    double elapsed_seconds;
} GenerationResult;

static int retry_first_run;
static const char *segment_tracker;

#define SEGMENT_FRAME_READY "\x01\x01" "READY" "\x01\x01"
#define REMOTE_SNAPSHOT_CHUNK (1u << 20)

static void sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000u),
                           (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
}

static const char *arg_value(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static int has_arg(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], name)) return 1;
    return 0;
}

static void usage(const char *program) {
    fprintf(stderr,
        "usage: %s --engine ID --model-dir DIR --model NAME "
        "--tracker HOST:PORT [--model-root HEX64 --tokenizer-root HEX64] "
        "((--prompt TEXT | --prompt-ids CSV) | --serve) "
        "[--expect-ids CSV] [--tokens N] [--context N] [--max-rows N] "
        "[--temperature F --top-p F --seed N] "
        "[--discovery-timeout-ms N] [--retry-first-run]\n",
        program);
}

static int parse_double(const char *text, double minimum, double maximum,
                        double *value) {
    if (!text || !*text || !value || minimum > maximum) return -1;
    char *end = NULL;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(parsed) ||
        parsed < minimum || parsed > maximum) return -1;
    *value = parsed;
    return 0;
}

static int parse_u64(const char *text, uint64_t *value) {
    if (!text || !*text || !value || text[0] == '-') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || end == text || *end) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static uint64_t sampler_seed(const char *requested) {
    uint64_t seed = 0;
    const char *text = requested ? requested : getenv("LUMABRI_SAMPLE_SEED");
    if (text && !parse_u64(text, &seed)) return seed;
    lmb_random((uint8_t *)&seed, sizeof seed);
    return seed;
}

static int model_identity_resolve(const char *tracker, const char *model,
                                  const char *model_root_text,
                                  const char *tokenizer_root_text,
                                  uint8_t model_root[32],
                                  uint8_t tokenizer_root[32]) {
    if (model_root_text || tokenizer_root_text) {
        if (!model_root_text || !tokenizer_root_text ||
            lmb_hex_root(model_root_text, model_root) ||
            lmb_hex_root(tokenizer_root_text, tokenizer_root))
            return -1;
        return 0;
    }
    LmbModelIdentity identity;
    if (lmb_model_identity_get(tracker, model, &identity)) return -1;
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
        if (bad) return -1;
    }
    memcpy(model_root, identity.root, 32);
    /* tokenizer.json belongs to the signed inventory represented by this
     * root, so the aggregate identity is also a stable tokenizer identity. */
    memcpy(tokenizer_root, identity.root, 32);
    return 0;
}

static int parse_ids(const char *text, int32_t **ids, size_t *count) {
    if (!text || !*text || !ids || !count) return -1;
    size_t n = 1;
    for (const char *p = text; *p; p++) if (*p == ',') n++;
    int32_t *values = malloc(n * sizeof *values);
    if (!values) return -1;
    const char *p = text;
    for (size_t i = 0; i < n; i++) {
        char *end = NULL;
        errno = 0;
        long value = strtol(p, &end, 10);
        if (errno || end == p || value < INT32_MIN || value > INT32_MAX ||
            (i + 1 < n ? *end != ',' : *end != '\0')) {
            free(values); return -1;
        }
        values[i] = (int32_t)value;
        p = end + (i + 1 < n);
    }
    *ids = values; *count = n;
    return 0;
}

static int select_chain(const LmbSegRouteSnapshot *snapshot, uint32_t layers,
                        uint32_t required_context, uint32_t required_rows,
                        RemoteSegment *chain, size_t *chain_count) {
    uint64_t fallback_layers[LMB_SEG_ROUTE_MAX] = {0};
    uint64_t load_cost[LMB_SEG_ROUTE_MAX] = {0};
    uint32_t hops[LMB_SEG_ROUTE_MAX] = {0};
    unsigned viable[LMB_SEG_ROUTE_MAX] = {0};
    for (uint32_t pass = 0; pass < snapshot->count; pass++) {
        int changed = 0;
        for (uint32_t i = 0; i < snapshot->count; i++) {
            const LmbSegRouteEntry *entry = &snapshot->entries[i];
            if (!(entry->transport & (LMB_SEG_TRANSPORT_DIRECT |
                                      LMB_SEG_TRANSPORT_RELAY)) ||
                entry->advert.layer_begin >= entry->advert.layer_end ||
                entry->advert.layer_end > layers ||
                entry->advert.max_context < required_context ||
                entry->advert.max_rows < required_rows) continue;
            uint64_t own_fallback =
                entry->advert.flags & LMB_SEG_ADVERT_FALLBACK
                    ? entry->advert.layer_end - entry->advert.layer_begin : 0;
            uint64_t own_load = (uint64_t)entry->advert.queue_depth +
                                entry->advert.inflight;
            if (entry->advert.layer_end == layers) {
                if (!viable[i] || own_fallback < fallback_layers[i] ||
                    (own_fallback == fallback_layers[i] && 1 < hops[i]) ||
                    (own_fallback == fallback_layers[i] && hops[i] == 1 &&
                     own_load < load_cost[i])) {
                    viable[i] = 1;
                    fallback_layers[i] = own_fallback;
                    load_cost[i] = own_load;
                    hops[i] = 1;
                    changed = 1;
                }
                continue;
            }
            int found = 0;
            uint64_t best_fallback = UINT64_MAX, best_load = UINT64_MAX;
            uint32_t best_hops = UINT32_MAX;
            for (uint32_t j = 0; j < snapshot->count; j++) {
                if (!viable[j] || snapshot->entries[j].advert.layer_begin !=
                                  entry->advert.layer_end) continue;
                uint64_t candidate_fallback = own_fallback + fallback_layers[j];
                uint64_t candidate_load = own_load + load_cost[j];
                uint32_t candidate_hops = 1 + hops[j];
                if (!found || candidate_fallback < best_fallback ||
                    (candidate_fallback == best_fallback &&
                     candidate_hops < best_hops) ||
                    (candidate_fallback == best_fallback &&
                     candidate_hops == best_hops &&
                     candidate_load < best_load)) {
                    found = 1;
                    best_fallback = candidate_fallback;
                    best_hops = candidate_hops;
                    best_load = candidate_load;
                }
            }
            if (found) {
                if (!viable[i] || best_fallback < fallback_layers[i] ||
                    (best_fallback == fallback_layers[i] &&
                     best_hops < hops[i]) ||
                    (best_fallback == fallback_layers[i] &&
                     best_hops == hops[i] && best_load < load_cost[i])) {
                    viable[i] = 1;
                    fallback_layers[i] = best_fallback;
                    hops[i] = best_hops;
                    load_cost[i] = best_load;
                    changed = 1;
                }
            }
        }
        if (!changed) break;
    }
    uint32_t cursor = 0;
    size_t count = 0;
    while (cursor < layers && count < LMB_SEG_ROUTE_MAX) {
        const LmbSegRouteEntry *best = NULL;
        for (uint32_t i = 0; i < snapshot->count; i++) {
            const LmbSegRouteEntry *candidate = &snapshot->entries[i];
            if (!viable[i] ||
                candidate->advert.layer_begin != cursor ||
                candidate->advert.layer_end > layers) continue;
            if (!best) { best = candidate; continue; }
            uint32_t best_index = (uint32_t)(best - snapshot->entries);
            if (fallback_layers[i] < fallback_layers[best_index] ||
                (fallback_layers[i] == fallback_layers[best_index] &&
                 hops[i] < hops[best_index]) ||
                (fallback_layers[i] == fallback_layers[best_index] &&
                 hops[i] == hops[best_index] &&
                 load_cost[i] < load_cost[best_index]) ||
                (fallback_layers[i] == fallback_layers[best_index] &&
                 hops[i] == hops[best_index] &&
                 load_cost[i] == load_cost[best_index] &&
                 (candidate->transport & LMB_SEG_TRANSPORT_DIRECT) &&
                 !(best->transport & LMB_SEG_TRANSPORT_DIRECT)) ||
                (fallback_layers[i] == fallback_layers[best_index] &&
                 hops[i] == hops[best_index] &&
                 load_cost[i] == load_cost[best_index] &&
                 !!(candidate->transport & LMB_SEG_TRANSPORT_DIRECT) ==
                    !!(best->transport & LMB_SEG_TRANSPORT_DIRECT) &&
                 candidate->advert.layer_end > best->advert.layer_end))
                best = candidate;
        }
        if (!best) return -1;
        memset(&chain[count], 0, sizeof chain[count]);
        chain[count].route = *best;
        chain[count].fd = -1;
        cursor = best->advert.layer_end;
        count++;
    }
    if (cursor != layers) return -1;
    *chain_count = count;
    return 0;
}

static int reply_ok(const LmbMsg *msg, uint32_t expected_op,
                    const LmbSegId *session, const LmbSegId *request,
                    uint64_t generation, LmbSegReply *reply) {
    if (msg->op != expected_op ||
        lmb_seg_reply_decode(msg->body, msg->body_len, reply) ||
        !lmb_seg_id_equal(&reply->session_id, session) ||
        !lmb_seg_id_equal(&reply->request_id, request) ||
        reply->route_generation != generation ||
        (reply->status != LMB_SEG_STATUS_OK &&
         reply->status != LMB_SEG_STATUS_DUPLICATE)) return -1;
    return 0;
}

static const char *segment_status_name(uint32_t status) {
    switch (status) {
    case LMB_SEG_STATUS_OK:            return "ok";
    case LMB_SEG_STATUS_DUPLICATE:     return "duplicate";
    case LMB_SEG_STATUS_BAD_REQUEST:   return "bad request — the executor's "
        "model identity, layer range, context, rows or numeric class does not "
        "match what the tracker advertised";
    case LMB_SEG_STATUS_NOT_FOUND:     return "session not found";
    case LMB_SEG_STATUS_STALE_OWNER:   return "stale owner — the route moved "
        "under this session";
    case LMB_SEG_STATUS_OUT_OF_ORDER:  return "out of order";
    case LMB_SEG_STATUS_CONFLICT:      return "conflict";
    case LMB_SEG_STATUS_EXPIRED:       return "expired";
    case LMB_SEG_STATUS_BUSY:          return "busy";
    case LMB_SEG_STATUS_UNSUPPORTED:   return "unsupported";
    case LMB_SEG_STATUS_QUOTA:         return "quota — the executor has no "
        "free session slot";
    case LMB_SEG_STATUS_NEEDS_RESTORE: return "needs restore";
    case LMB_SEG_STATUS_INTERNAL:      return "internal executor failure — "
        "look at that peer's log";
    default:                           return "unknown status";
    }
}

static int remote_relay_request(RemoteSegment *remote, uint32_t op,
                                const void *body, uint32_t body_len,
                                const void *pay, uint32_t pay_len,
                                LmbMsg *response) {
    if (!segment_tracker ||
        !(remote->route.transport & LMB_SEG_TRANSPORT_RELAY)) return -1;
    LmbBuf envelope = {0};
    if (lmb_buf_str(&envelope, remote->route.advert.peer_name) ||
        lmb_buf_u32(&envelope, op) || lmb_buf_u32(&envelope, body_len) ||
        lmb_buf_bytes(&envelope, body, body_len)) {
        free(envelope.p); return -1;
    }
    LmbMsg outer = {0};
    int bad = lmb_request_pay(segment_tracker, LMB_RSEG,
                              envelope.p, (uint32_t)envelope.len,
                              pay, pay_len, &outer);
    free(envelope.p);
    if (bad || outer.op != LMB_RSEG_R) {
        lmb_msg_free(&outer); return -1;
    }
    LmbCur cursor = { outer.body, outer.body_len, 0 };
    uint32_t inner_op = 0, inner_body_len = 0;
    if (lmb_cur_u32(&cursor, &inner_op) ||
        lmb_cur_u32(&cursor, &inner_body_len) ||
        inner_body_len != cursor.len - cursor.off || inner_op != op + 1u ||
        !lmb_frame_shape_ok(inner_op, inner_body_len, outer.pay_len)) {
        lmb_msg_free(&outer); return -1;
    }
    response->op = inner_op;
    response->body_len = inner_body_len;
    response->pay_len = outer.pay_len;
    response->body = malloc(inner_body_len ? inner_body_len : 1u);
    response->pay = malloc(outer.pay_len ? outer.pay_len : 1u);
    if (!response->body || !response->pay) {
        lmb_msg_free(response); lmb_msg_free(&outer); return -1;
    }
    if (inner_body_len)
        memcpy(response->body, cursor.p + cursor.off, inner_body_len);
    if (outer.pay_len) memcpy(response->pay, outer.pay, outer.pay_len);
    lmb_msg_free(&outer);
    return 0;
}

static int remote_request(RemoteSegment *remote, uint32_t op,
                          const void *body, uint32_t body_len,
                          const void *pay, uint32_t pay_len,
                          LmbMsg *response) {
    if (!remote->direct_failed &&
        (remote->route.transport & LMB_SEG_TRANSPORT_DIRECT)) {
        remote->transport_error[0] = 0;
        if (remote->fd < 0) {
            remote->fd = lmb_connect(remote->route.advert.addr);
            if (remote->fd < 0)
                snprintf(remote->transport_error,
                         sizeof remote->transport_error,
                         "cannot reach %s at %s: %s",
                         remote->route.advert.peer_name,
                         remote->route.advert.addr, lmb_connect_why());
        }
        if (remote->fd >= 0) {
            if (lmb_send(remote->fd, op, body, body_len, pay, pay_len))
                snprintf(remote->transport_error,
                         sizeof remote->transport_error,
                         "%s at %s closed the connection while sending op %u",
                         remote->route.advert.peer_name,
                         remote->route.advert.addr, op);
            else if (lmb_recv(remote->fd, response))
                snprintf(remote->transport_error,
                         sizeof remote->transport_error,
                         "%s at %s sent no reply to op %u",
                         remote->route.advert.peer_name,
                         remote->route.advert.addr, op);
            else return 0;
        }
        lmb_msg_free(response);
        if (remote->fd >= 0) lmb_close(remote->fd);
        remote->fd = -1;
        remote->direct_failed = 1;
    }
    if (!(remote->route.transport & LMB_SEG_TRANSPORT_RELAY)) return -1;
    if (!remote_relay_request(remote, op, body, body_len,
                              pay, pay_len, response)) return 0;
    size_t used = strlen(remote->transport_error);
    snprintf(remote->transport_error + used,
             sizeof remote->transport_error - used,
             "%stracker relay for %s is unavailable",
             used ? "; " : "", remote->route.advert.peer_name);
    return -1;
}

/* One message for four causes — a filtered port, a dead executor, a rejected
 * OPEN and a malformed reply — costs an operator the whole diagnosis, because
 * the firewall fix and the compatibility fix look identical from here. Say
 * which one happened. */
static int remote_open(RemoteSegment *remote, const LmbSegId *session_id,
                       const uint8_t model_root[32],
                       const uint8_t tokenizer_root[32], uint32_t context,
                       uint32_t max_rows, char *why, size_t why_size) {
    const LmbSegAdvert *advert = &remote->route.advert;
    LmbSegOpen *open = &remote->open;
    memset(open, 0, sizeof *open);
    open->session_id = *session_id;
    lmb_random(open->request_id.bytes, sizeof open->request_id.bytes);
    open->owner = remote->route.owner;
    memcpy(open->model_root, model_root, sizeof open->model_root);
    memcpy(open->tokenizer_root, tokenizer_root, sizeof open->tokenizer_root);
    open->layer_begin = advert->layer_begin;
    open->layer_end = advert->layer_end;
    open->context_tokens = context;
    open->max_rows = max_rows;
    open->state_dtype = advert->state_dtype;
    open->state_width = advert->state_width;
    open->ttl_ms = LMB_SEG_MAX_TTL_MS;
    open->capabilities = advert->capabilities;
    snprintf(open->engine_id, sizeof open->engine_id, "%s", advert->engine_id);
    snprintf(open->state_schema, sizeof open->state_schema, "%s",
             advert->state_schema);
    snprintf(open->numeric_class, sizeof open->numeric_class, "%s",
             advert->numeric_class);
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (lmb_seg_open_encode(open, &body, &body_len)) {
        snprintf(why, why_size, "cannot encode the OPEN request");
        return -1;
    }
    LmbMsg msg = {0};
    LmbSegReply reply;
    if (remote_request(remote, LMB_SEG_OPEN, body, body_len,
                       NULL, 0, &msg)) {
        snprintf(why, why_size, "%s", remote->transport_error[0]
                 ? remote->transport_error : "no usable Segment transport");
        free(body);
        lmb_msg_free(&msg);
        return -1;
    }
    free(body);
    if (msg.op == LMB_ERR) {
        snprintf(why, why_size, "%s refused OPEN: %.*s",
                 advert->peer_name, (int)msg.body_len,
                 msg.body ? (const char *)msg.body : "");
    } else if (msg.op != LMB_SEG_OPEN_R ||
               lmb_seg_reply_decode(msg.body, msg.body_len, &reply)) {
        snprintf(why, why_size, "%s answered OPEN with an unreadable "
                 "message (op %u)", advert->peer_name, msg.op);
    } else if (reply.status != LMB_SEG_STATUS_OK &&
               reply.status != LMB_SEG_STATUS_DUPLICATE) {
        snprintf(why, why_size, "%s rejected OPEN [%u:%u]: %s",
                 advert->peer_name, advert->layer_begin, advert->layer_end,
                 segment_status_name(reply.status));
    } else if (reply_ok(&msg, LMB_SEG_OPEN_R, session_id, &open->request_id,
                        open->owner.route_generation, &reply) || msg.pay_len) {
        /* Accepted, but not for this session/request/route: the placement
         * moved while we were opening, or the peer is answering someone
         * else's traffic. Either way this chain is not usable as it stands. */
        snprintf(why, why_size, "%s at %s accepted OPEN for a different "
                 "session or route generation", advert->peer_name,
                 advert->addr);
    } else {
        lmb_msg_free(&msg);
        remote->sequence = reply.next_sequence;
        remote->position = reply.next_position;
        remote->opened = 1;
        return 0;
    }
    lmb_msg_free(&msg);
    if (remote->fd >= 0) lmb_close(remote->fd);
    remote->fd = -1;
    return -1;
}

static int remote_run(RemoteSegment *remote, const int32_t *tokens,
                      uint32_t rows, const void *input, size_t bytes,
                      void *output) {
    LmbSegRun run;
    memset(&run, 0, sizeof run);
    run.session_id = remote->open.session_id;
    lmb_random(run.request_id.bytes, sizeof run.request_id.bytes);
    run.owner = remote->open.owner;
    run.sequence = remote->sequence;
    run.position = remote->position;
    run.rows = rows;
    run.token_count = rows;
    run.token_ids = tokens;
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (lmb_seg_run_encode(&run, &body, &body_len) || bytes > UINT32_MAX)
        return -1;
    LmbMsg msg = {0};
    LmbSegReply reply;
    int bad = remote_request(remote, LMB_SEG_RUN, body, body_len,
                             input, (uint32_t)bytes, &msg) ||
          reply_ok(&msg, LMB_SEG_RUN_R, &run.session_id, &run.request_id,
                   run.owner.route_generation, &reply) ||
          msg.pay_len != bytes;
    if (!bad) {
        memcpy(output, msg.pay, bytes);
        if (reply.next_sequence != run.sequence + 1 ||
            reply.next_position != run.position + rows) bad = 1;
        else {
            remote->sequence = reply.next_sequence;
            remote->position = reply.next_position;
        }
    }
    lmb_msg_free(&msg);
    if (!bad && retry_first_run) {
        retry_first_run = 0;
        memset(&msg, 0, sizeof msg);
        bad = remote_request(remote, LMB_SEG_RUN, body, body_len,
                             input, (uint32_t)bytes, &msg);
        LmbSegReply duplicate;
        if (!bad) bad =
            reply_ok(&msg, LMB_SEG_RUN_R, &run.session_id, &run.request_id,
                     run.owner.route_generation, &duplicate) ||
            duplicate.status != LMB_SEG_STATUS_DUPLICATE ||
            duplicate.next_sequence != remote->sequence ||
            duplicate.next_position != remote->position ||
            msg.pay_len != bytes || memcmp(msg.pay, output, bytes);
        lmb_msg_free(&msg);
    }
    free(body);
    return bad ? -1 : 0;
}

typedef struct {
    uint8_t *data;
    size_t bytes;
    uint64_t sequence;
    uint64_t position;
} RemoteCheckpoint;

static int remote_snapshot(RemoteSegment *remote, RemoteCheckpoint *checkpoint) {
    memset(checkpoint, 0, sizeof *checkpoint);
    LmbSegTransfer transfer;
    memset(&transfer, 0, sizeof transfer);
    transfer.session_id = remote->open.session_id;
    lmb_random(transfer.request_id.bytes, sizeof transfer.request_id.bytes);
    transfer.owner = remote->open.owner;
    transfer.sequence = remote->sequence;
    transfer.position = remote->position;
    transfer.flags = LMB_SEG_XFER_BEGIN | LMB_SEG_XFER_END;
    uint64_t offset = 0, snapshot_size = 0;
    uint8_t *data = NULL;
    for (;;) {
        uint8_t *body = NULL;
        uint32_t body_len = 0;
        if (lmb_seg_transfer_encode(&transfer, &body, &body_len)) {
            free(data); return -1;
        }
        LmbMsg message = {0};
        int bad = remote_request(remote, LMB_SEG_SNAPSHOT, body, body_len,
                                 NULL, 0, &message);
        free(body);
        LmbSegTransferReply reply;
        if (!bad)
            bad = message.op != LMB_SEG_SNAPSHOT_R ||
                  lmb_seg_transfer_reply_decode(message.body,
                                                message.body_len, &reply) ||
                  !lmb_seg_id_equal(&reply.reply.session_id,
                                    &transfer.session_id) ||
                  !lmb_seg_id_equal(&reply.reply.request_id,
                                    &transfer.request_id) ||
                  reply.reply.status != LMB_SEG_STATUS_OK ||
                  reply.reply.route_generation !=
                      transfer.owner.route_generation ||
                  reply.offset != offset || reply.chunk_len != message.pay_len;
        if (!bad && !data) {
            size_t limit = (size_t)lmb_env_int(
                "LUMABRI_SEGMENT_MAX_SNAPSHOT_MB", 2048, 1, 32768) << 20;
            if (!reply.snapshot_size || reply.snapshot_size > limit)
                bad = 1;
            else {
                snapshot_size = reply.snapshot_size;
                data = malloc((size_t)snapshot_size);
                if (!data) bad = 1;
            }
        }
        if (!bad && (reply.snapshot_size != snapshot_size ||
                     reply.offset > snapshot_size ||
                     reply.chunk_len > snapshot_size - reply.offset)) bad = 1;
        /* A non-final empty chunk would never advance offset and could keep a
         * broken or malicious executor in this loop forever. */
        if (!bad && !reply.chunk_len &&
            !(reply.transfer_flags & LMB_SEG_XFER_END)) bad = 1;
        if (!bad && reply.chunk_len)
            memcpy(data + reply.offset, message.pay, reply.chunk_len);
        if (!bad) offset += reply.chunk_len;
        int complete = !bad && (reply.transfer_flags & LMB_SEG_XFER_END);
        lmb_msg_free(&message);
        if (bad || (complete && offset != snapshot_size)) {
            free(data); return -1;
        }
        if (complete) break;
        transfer.snapshot_size = snapshot_size;
        transfer.offset = offset;
        uint64_t remaining = snapshot_size - offset;
        transfer.chunk_len = (uint32_t)(remaining < REMOTE_SNAPSHOT_CHUNK
                                      ? remaining : REMOTE_SNAPSHOT_CHUNK);
        transfer.flags = transfer.chunk_len == remaining ? LMB_SEG_XFER_END : 0;
    }
    checkpoint->data = data;
    checkpoint->bytes = (size_t)snapshot_size;
    checkpoint->sequence = remote->sequence;
    checkpoint->position = remote->position;
    return 0;
}

static int remote_restore(RemoteSegment *remote, const RemoteCheckpoint *source) {
    if (!source || !source->data || !source->bytes) return -1;
    LmbSegTransfer transfer;
    memset(&transfer, 0, sizeof transfer);
    transfer.session_id = remote->open.session_id;
    lmb_random(transfer.request_id.bytes, sizeof transfer.request_id.bytes);
    transfer.owner = remote->open.owner;
    transfer.sequence = source->sequence;
    transfer.position = source->position;
    transfer.snapshot_size = source->bytes;
    for (size_t offset = 0; offset < source->bytes; ) {
        size_t remaining = source->bytes - offset;
        size_t chunk = remaining < REMOTE_SNAPSHOT_CHUNK
                     ? remaining : REMOTE_SNAPSHOT_CHUNK;
        transfer.offset = offset;
        transfer.chunk_len = (uint32_t)chunk;
        transfer.flags = (!offset ? LMB_SEG_XFER_BEGIN : 0) |
                         (chunk == remaining ? LMB_SEG_XFER_END : 0);
        uint8_t *body = NULL;
        uint32_t body_len = 0;
        if (lmb_seg_transfer_encode(&transfer, &body, &body_len)) return -1;
        LmbMsg message = {0};
        int bad = remote_request(remote, LMB_SEG_RESTORE, body, body_len,
                                 source->data + offset, (uint32_t)chunk,
                                 &message);
        free(body);
        LmbSegReply reply;
        if (!bad)
            bad = reply_ok(&message, LMB_SEG_RESTORE_R,
                           &transfer.session_id, &transfer.request_id,
                           transfer.owner.route_generation, &reply) ||
                  message.pay_len;
        lmb_msg_free(&message);
        if (bad) return -1;
        offset += chunk;
        if (offset == source->bytes) {
            if (reply.next_sequence != source->sequence ||
                reply.next_position != source->position) return -1;
            remote->sequence = reply.next_sequence;
            remote->position = reply.next_position;
        }
    }
    return 0;
}

static void remote_close(RemoteSegment *remote) {
    if (!remote->opened) return;
    LmbSegControl control;
    memset(&control, 0, sizeof control);
    control.session_id = remote->open.session_id;
    lmb_random(control.request_id.bytes, sizeof control.request_id.bytes);
    control.owner = remote->open.owner;
    control.sequence = remote->sequence;
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (!lmb_seg_control_encode(&control, &body, &body_len)) {
        LmbMsg reply = {0};
        if (!remote_request(remote, LMB_SEG_CLOSE, body, body_len,
                            NULL, 0, &reply))
            lmb_msg_free(&reply);
    }
    free(body);
    if (remote->fd >= 0) lmb_close(remote->fd);
    remote->fd = -1;
    remote->opened = 0;
}

static void remote_dispose(RemoteSegment *remote) {
    remote_close(remote);
    free(remote->snapshot);
    remote->snapshot = NULL;
    remote->snapshot_bytes = 0;
}

static void conversation_reset(SegmentConversation *conversation) {
    if (!conversation) return;
    for (size_t i = 0; i < conversation->chain_count; i++)
        remote_dispose(&conversation->chain[i]);
    free(conversation->committed_tokens);
    memset(conversation, 0, sizeof *conversation);
}

static int conversation_route_equal(const SegmentConversation *conversation,
                                    const RemoteSegment *desired,
                                    size_t desired_count,
                                    uint64_t generation) {
    if (!conversation || !conversation->active ||
        conversation->route_generation != generation ||
        conversation->chain_count != desired_count) return 0;
    for (size_t i = 0; i < desired_count; i++) {
        const LmbSegAdvert *a = &conversation->chain[i].route.advert;
        const LmbSegAdvert *b = &desired[i].route.advert;
        if (a->layer_begin != b->layer_begin || a->layer_end != b->layer_end ||
            strcmp(a->peer_name, b->peer_name) || strcmp(a->addr, b->addr))
            return 0;
    }
    return 1;
}

static int chain_run(RemoteSegment *chain, size_t count,
                     const int32_t *tokens, uint32_t rows,
                     uint8_t **first, uint8_t **second, size_t bytes) {
    uint8_t *input = *first, *output = *second;
    for (size_t i = 0; i < count; i++) {
        if (remote_run(&chain[i], tokens, rows, input, bytes, output))
            return -(int)i - 1;
        uint8_t *swap = input; input = output; output = swap;
    }
    *first = input;
    *second = output;
    return 0;
}

static int conversation_checkpoint(SegmentConversation *conversation,
                                   size_t token_count) {
    if (!conversation || !conversation->active) return -1;
    RemoteCheckpoint checkpoints[LMB_SEG_ROUTE_MAX];
    memset(checkpoints, 0, sizeof checkpoints);
    size_t completed = 0;
    for (; completed < conversation->chain_count; completed++)
        if (remote_snapshot(&conversation->chain[completed],
                            &checkpoints[completed])) break;
    if (completed != conversation->chain_count) {
        for (size_t i = 0; i < completed; i++) free(checkpoints[i].data);
        return -1;
    }
    for (size_t i = 0; i < conversation->chain_count; i++) {
        RemoteSegment *remote = &conversation->chain[i];
        free(remote->snapshot);
        remote->snapshot = checkpoints[i].data;
        remote->snapshot_bytes = checkpoints[i].bytes;
        remote->snapshot_sequence = checkpoints[i].sequence;
        remote->snapshot_position = checkpoints[i].position;
    }
    conversation->checkpoint_count = token_count;
    return 0;
}

static int route_same_range(const LmbSegRouteEntry *a,
                            const LmbSegRouteEntry *b,
                            uint32_t context, uint32_t max_rows) {
    return a->advert.layer_begin == b->advert.layer_begin &&
           a->advert.layer_end == b->advert.layer_end &&
           a->advert.max_context >= context &&
           a->advert.max_rows >= max_rows &&
           a->advert.state_dtype == b->advert.state_dtype &&
           a->advert.state_width == b->advert.state_width &&
           !strcmp(a->advert.engine_id, b->advert.engine_id) &&
           !strcmp(a->advert.state_schema, b->advert.state_schema) &&
           !strcmp(a->advert.numeric_class, b->advert.numeric_class) &&
           (a->transport & (LMB_SEG_TRANSPORT_DIRECT |
                            LMB_SEG_TRANSPORT_RELAY));
}

/* Serializing opaque KV/recurrent state is useful only when the immutable
 * route snapshot contains somewhere to restore it. Avoid copying and sending
 * a potentially large checkpoint on an origin-only swarm where failover is
 * impossible anyway. When any selected range has a replica, all ranges are
 * snapshotted so a replacement chain can be restored at one common token. */
static int conversation_has_replica(
    const SegmentConversation *conversation,
    const LmbSegRouteSnapshot *snapshot,
    uint32_t context, uint32_t max_rows) {
    if (!conversation || !snapshot) return 0;
    for (size_t i = 0; i < conversation->chain_count; i++)
        for (uint32_t j = 0; j < snapshot->count; j++) {
            const LmbSegRouteEntry *current = &conversation->chain[i].route;
            const LmbSegRouteEntry *candidate = &snapshot->entries[j];
            if (strcmp(candidate->advert.peer_name,
                       current->advert.peer_name) &&
                route_same_range(candidate, current, context, max_rows))
                return 1;
        }
    return 0;
}

static int conversation_recover(
    SegmentConversation *conversation,
    const LmbSegRouteSnapshot *snapshot, size_t failed_index,
    ColiEdgeEngine *edge, const ColiEdgeCapabilities *cap,
    const uint8_t model_root[32], const uint8_t tokenizer_root[32],
    uint32_t context, uint32_t max_rows,
    const int32_t *prompt_tokens, size_t prompt_replayed,
    const int32_t *generated, size_t generated_replayed,
    uint8_t *buffer_a, uint8_t *buffer_b,
    char *error, size_t error_size) {
    if (!conversation || !conversation->active || !snapshot ||
        failed_index >= conversation->chain_count) return -1;
    RemoteSegment replacement[LMB_SEG_ROUTE_MAX];
    memset(replacement, 0, sizeof replacement);
    for (size_t i = 0; i < conversation->chain_count; i++) {
        const LmbSegRouteEntry *current = &conversation->chain[i].route;
        const LmbSegRouteEntry *chosen = current;
        if (i == failed_index) {
            chosen = NULL;
            for (uint32_t j = 0; j < snapshot->count; j++) {
                const LmbSegRouteEntry *candidate = &snapshot->entries[j];
                if (!strcmp(candidate->advert.peer_name,
                            current->advert.peer_name) ||
                    !route_same_range(candidate, current,
                                      context, max_rows)) continue;
                if (!chosen ||
                    ((candidate->transport & LMB_SEG_TRANSPORT_DIRECT) &&
                     !(chosen->transport & LMB_SEG_TRANSPORT_DIRECT)) ||
                    ((candidate->advert.flags & LMB_SEG_ADVERT_FALLBACK) == 0 &&
                     (chosen->advert.flags & LMB_SEG_ADVERT_FALLBACK)))
                    chosen = candidate;
            }
            if (!chosen) {
                snprintf(error, error_size,
                         "Segment peer failed and no compatible replica exists");
                return -1;
            }
        }
        replacement[i].route = *chosen;
        replacement[i].fd = -1;
    }

    LmbSegId session_id;
    lmb_random(session_id.bytes, sizeof session_id.bytes);
    size_t opened = 0;
    for (; opened < conversation->chain_count; opened++) {
        if (remote_open(&replacement[opened], &session_id,
                        model_root, tokenizer_root, context, max_rows,
                        error, error_size)) break;
        if (conversation->checkpoint_count) {
            RemoteSegment *old = &conversation->chain[opened];
            RemoteCheckpoint checkpoint = {
                old->snapshot, old->snapshot_bytes,
                old->snapshot_sequence, old->snapshot_position,
            };
            if (!checkpoint.data ||
                checkpoint.position != conversation->checkpoint_count ||
                remote_restore(&replacement[opened], &checkpoint)) break;
        }
    }
    if (opened != conversation->chain_count) {
        for (size_t i = 0; i <= opened && i < conversation->chain_count; i++)
            remote_dispose(&replacement[i]);
        snprintf(error, error_size,
                 "Segment replica could not restore the last checkpoint");
        return -1;
    }

    size_t total = prompt_replayed + generated_replayed;
    int32_t *history = total ? malloc(total * sizeof *history) : NULL;
    if (total && !history) goto recovery_failed;
    if (prompt_replayed)
        memcpy(history, prompt_tokens, prompt_replayed * sizeof *history);
    if (generated_replayed)
        memcpy(history + prompt_replayed, generated,
               generated_replayed * sizeof *history);
    if (conversation->checkpoint_count > total) {
        free(history); goto recovery_failed;
    }
    for (size_t offset = conversation->checkpoint_count;
         offset < total; offset += max_rows) {
        uint32_t rows = (uint32_t)(total - offset);
        if (rows > max_rows) rows = max_rows;
        size_t bytes = lmb_state_bytes(rows, cap->state_width, cap->state_dtype);
        ColiEdgeEmbedRequest embed = {
            .struct_size = sizeof embed, .rows = rows,
            .token_ids = history + offset, .token_count = rows,
            .output = buffer_a, .output_bytes = bytes,
        };
        if (coli_edge_embed(edge, &embed, error, error_size)) {
            free(history); goto recovery_failed;
        }
        uint8_t *first = buffer_a, *second = buffer_b;
        if (chain_run(replacement, conversation->chain_count,
                      history + offset, rows, &first, &second, bytes)) {
            free(history); goto recovery_failed;
        }
    }
    free(history);
    for (size_t i = 0; i < conversation->chain_count; i++) {
        RemoteSegment *old = &conversation->chain[i];
        replacement[i].snapshot = old->snapshot;
        replacement[i].snapshot_bytes = old->snapshot_bytes;
        replacement[i].snapshot_sequence = old->snapshot_sequence;
        replacement[i].snapshot_position = old->snapshot_position;
        old->snapshot = NULL; old->snapshot_bytes = 0;
        remote_dispose(old);
        *old = replacement[i];
    }
    conversation->route_generation = snapshot->route_generation;
    fprintf(stderr, "[lumabri] Segment failover: restored checkpoint at token "
                    "%zu and replayed %zu token%s\n",
            conversation->checkpoint_count,
            total - conversation->checkpoint_count,
            total - conversation->checkpoint_count == 1 ? "" : "s");
    return 0;

recovery_failed:
    for (size_t i = 0; i < conversation->chain_count; i++)
        remote_dispose(&replacement[i]);
    snprintf(error, error_size,
             "Segment checkpoint replay failed on the replacement chain");
    return -1;
}

static double monotonic_seconds(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec * 1e-9;
}

static void generation_result_free(GenerationResult *result) {
    if (!result) return;
    free(result->text);
    free(result->tokens);
    memset(result, 0, sizeof *result);
}

static int segment_generate(ColiEdgeEngine *edge,
                            const ColiEdgeCapabilities *cap,
                            RemoteSegment *chain, size_t chain_count,
                            const uint8_t model_root[32],
                            const uint8_t tokenizer_root[32],
                            uint32_t context, uint32_t max_rows,
                            const char *prompt, size_t prompt_bytes,
                            const int32_t *provided_tokens,
                            size_t provided_count, uint32_t wanted_tokens,
                            double temperature, double top_p,
                            LmbSampler *sampler,
                            SegmentConversation *conversation,
                            const LmbSegRouteSnapshot *route_snapshot,
                            uint64_t route_generation,
                            GenerationResult *result,
                            char *error, size_t error_size) {
    memset(result, 0, sizeof *result);
    /* Own the reason from here on: a caller's placeholder left in place would
     * report the previous stage's failure for this one. */
    if (error && error_size) error[0] = 0;
    double started = monotonic_seconds();
    size_t opened = 0;
    int32_t *prompt_tokens = NULL;
    int32_t *generated = NULL;
    uint8_t *buffer_a = NULL, *buffer_b = NULL;
    float *logits = NULL;
    RemoteSegment *active_chain = chain;
    size_t active_count = chain_count;
    size_t prefilled = 0;
    int rc = -1;

    size_t prompt_count = provided_count;
    if (provided_tokens) {
        if (!provided_count || provided_count > SIZE_MAX / sizeof *prompt_tokens)
            goto cleanup;
        prompt_tokens = malloc(provided_count * sizeof *prompt_tokens);
        if (!prompt_tokens) goto cleanup;
        memcpy(prompt_tokens, provided_tokens,
               provided_count * sizeof *prompt_tokens);
    } else {
        if (!prompt ||
            coli_edge_tokenize(edge, prompt, prompt_bytes, NULL, 0,
                               &prompt_count, error, error_size) ||
            !prompt_count || prompt_count > SIZE_MAX / sizeof *prompt_tokens)
            goto cleanup;
        prompt_tokens = malloc(prompt_count * sizeof *prompt_tokens);
        size_t actual_count = 0;
        if (!prompt_tokens ||
            coli_edge_tokenize(edge, prompt, prompt_bytes, prompt_tokens,
                               prompt_count, &actual_count,
                               error, error_size) ||
            actual_count != prompt_count)
            goto cleanup;
    }

    if (conversation) {
        int same_route = conversation_route_equal(conversation, chain,
                                                  chain_count,
                                                  route_generation);
        int same_prefix = same_route &&
            conversation->committed_count <= prompt_count &&
            (!conversation->committed_count ||
             !memcmp(conversation->committed_tokens, prompt_tokens,
                     conversation->committed_count * sizeof *prompt_tokens));
        if (!same_prefix) {
            conversation_reset(conversation);
            memcpy(conversation->chain, chain,
                   chain_count * sizeof *conversation->chain);
            conversation->chain_count = chain_count;
            conversation->route_generation = route_generation;
            for (size_t i = 0; i < chain_count; i++)
                conversation->chain[i].fd = -1;
        } else {
            prefilled = conversation->committed_count;
            if (prefilled)
                fprintf(stderr, "[lumabri] Segment KV reuse: %zu/%zu prompt "
                        "tokens already resident\n", prefilled, prompt_count);
        }
        active_chain = conversation->chain;
        active_count = conversation->chain_count;
    }

    if (!conversation || !conversation->active) {
        LmbSegId session_id;
        lmb_random(session_id.bytes, sizeof session_id.bytes);
        for (; opened < active_count; opened++) {
            active_chain[opened].fd = -1;
            if (remote_open(&active_chain[opened], &session_id, model_root,
                            tokenizer_root, context, max_rows,
                            error, error_size))
                goto cleanup;
        }
        if (conversation) conversation->active = 1;
    } else {
        opened = active_count;
    }
    if (prompt_count > context || wanted_tokens > context - prompt_count) {
        snprintf(error, error_size, "prompt plus output exceeds context (%u)",
                 context);
        goto cleanup;
    }
    if (prefilled >= prompt_count) {
        /* We do not keep the last hidden row at Edge. A normal chat turn
         * always appends role/user tokens; an identical resubmission safely
         * rebuilds instead of sampling from stale or duplicated state. */
        if (conversation) {
            conversation_reset(conversation);
            snprintf(error, error_size, "Segment prompt did not extend the "
                     "resident conversation; retry after session rebuild");
        }
        goto cleanup;
    }
    generated = malloc((size_t)wanted_tokens * sizeof *generated);
    if (temperature > 0.0) {
        if (!(cap->flags & COLI_EDGE_CAP_LOGITS) || !sampler) {
            snprintf(error, error_size,
                     "Colibri Edge logits are unavailable for sampling");
            goto cleanup;
        }
        logits = malloc((size_t)cap->vocab_size * sizeof *logits);
    }
    size_t max_bytes = lmb_state_bytes(max_rows, cap->state_width,
                                       cap->state_dtype);
    buffer_a = max_bytes ? malloc(max_bytes) : NULL;
    buffer_b = max_bytes ? malloc(max_bytes) : NULL;
    if (!generated || !buffer_a || !buffer_b ||
        (temperature > 0.0 && !logits)) {
        snprintf(error, error_size, "out of memory preparing Segment run");
        goto cleanup;
    }
    uint8_t *final_state = NULL;
    uint32_t final_rows = 0;
    for (size_t offset = prefilled; offset < prompt_count; offset += max_rows) {
        uint32_t rows = (uint32_t)(prompt_count - offset);
        if (rows > max_rows) rows = max_rows;
        size_t bytes = lmb_state_bytes(rows, cap->state_width,
                                       cap->state_dtype);
        ColiEdgeEmbedRequest embed = {
            .struct_size = sizeof embed, .rows = rows,
            .token_ids = prompt_tokens + offset, .token_count = rows,
            .output = buffer_a, .output_bytes = bytes,
        };
        if (coli_edge_embed(edge, &embed, error, error_size)) goto cleanup;
        uint8_t *first = buffer_a, *second = buffer_b;
        int failed = chain_run(active_chain, active_count,
                               prompt_tokens + offset, rows,
                               &first, &second, bytes);
        if (failed && (!conversation || !route_snapshot ||
            conversation_recover(conversation, route_snapshot,
                                 (size_t)(-failed - 1), edge, cap,
                                 model_root, tokenizer_root, context, max_rows,
                                 prompt_tokens, offset, NULL, 0,
                                 buffer_a, buffer_b, error, error_size))) {
            if (!error[0])
                snprintf(error, error_size,
                         "Segment peer failed and recovery was unavailable");
            goto cleanup;
        }
        if (failed) {
            if (coli_edge_embed(edge, &embed, error, error_size)) goto cleanup;
            first = buffer_a; second = buffer_b;
            if (chain_run(active_chain, active_count,
                          prompt_tokens + offset, rows,
                          &first, &second, bytes)) goto cleanup;
        }
        final_state = first;
        final_rows = rows;
        if (first != buffer_a) {
            uint8_t *swap = buffer_a; buffer_a = buffer_b; buffer_b = swap;
        }
    }
    size_t element = cap->state_dtype == COLI_EDGE_DTYPE_F32 ? 4u : 2u;
    const uint8_t *last = final_state +
        (size_t)(final_rows - 1) * cap->state_width * element;
    size_t state_row_bytes = (size_t)cap->state_width * element;
    ColiEdgeSelectRequest select = {
        .struct_size = sizeof select, .rows = 1,
        .input = last, .input_bytes = state_row_bytes,
        .token_ids = generated, .token_capacity = 1,
    };
    ColiEdgeLogitsRequest logits_request = {
        .struct_size = sizeof logits_request, .rows = 1,
        .input = last, .input_bytes = state_row_bytes,
        .logits = logits, .logits_capacity = cap->vocab_size,
    };
    if (temperature > 0.0) {
        if (coli_edge_logits(edge, &logits_request, error, error_size) ||
            lmb_sample_logits(sampler, logits, cap->vocab_size,
                              temperature, top_p, generated)) {
            if (!error[0]) snprintf(error, error_size, "sampling failed");
            goto cleanup;
        }
    } else if (coli_edge_select(edge, &select, error, error_size)) goto cleanup;
    size_t generated_count = 1;
    while (generated_count < wanted_tokens &&
           generated[generated_count - 1] != cap->eos_token_id) {
        int32_t token = generated[generated_count - 1];
        size_t bytes = lmb_state_bytes(1, cap->state_width, cap->state_dtype);
        ColiEdgeEmbedRequest embed = {
            .struct_size = sizeof embed, .rows = 1,
            .token_ids = &token, .token_count = 1,
            .output = buffer_a, .output_bytes = bytes,
        };
        if (coli_edge_embed(edge, &embed, error, error_size)) goto cleanup;
        uint8_t *first = buffer_a, *second = buffer_b;
        int failed = chain_run(active_chain, active_count, &token, 1,
                               &first, &second, bytes);
        if (failed && (!conversation || !route_snapshot ||
            conversation_recover(conversation, route_snapshot,
                                 (size_t)(-failed - 1), edge, cap,
                                 model_root, tokenizer_root, context, max_rows,
                                 prompt_tokens, prompt_count,
                                 generated, generated_count - 1,
                                 buffer_a, buffer_b, error, error_size))) {
            if (!error[0])
                snprintf(error, error_size,
                         "Segment peer failed and recovery was unavailable");
            goto cleanup;
        }
        if (failed) {
            if (coli_edge_embed(edge, &embed, error, error_size)) goto cleanup;
            first = buffer_a; second = buffer_b;
            if (chain_run(active_chain, active_count, &token, 1,
                          &first, &second, bytes)) goto cleanup;
        }
        select.input = first;
        select.token_ids = generated + generated_count;
        logits_request.input = first;
        if (temperature > 0.0) {
            if (coli_edge_logits(edge, &logits_request, error, error_size) ||
                lmb_sample_logits(sampler, logits, cap->vocab_size,
                                  temperature, top_p,
                                  generated + generated_count)) {
                if (!error[0]) snprintf(error, error_size, "sampling failed");
                goto cleanup;
            }
        } else if (coli_edge_select(edge, &select, error, error_size))
            goto cleanup;
        generated_count++;
    }
    /* The token that stopped generation is a control marker, not text. The
     * monolithic engines never put it in the reply and the TUI must not
     * either — a user was shown a literal end-of-sentence marker at the end
     * of every Segment answer. The raw IDs still carry it: the oracle gate
     * compares token IDs, not rendered text. */
    size_t text_tokens = generated_count;
    while (text_tokens && generated[text_tokens - 1] == cap->eos_token_id)
        text_tokens--;
    size_t text_bytes = 0;
    if (text_tokens &&
        coli_edge_detokenize(edge, generated, text_tokens, NULL, 0,
                             &text_bytes, error, error_size)) goto cleanup;
    char *text = malloc(text_bytes + 1);
    if (!text || (text_tokens &&
                  coli_edge_detokenize(edge, generated, text_tokens,
                                       text, text_bytes + 1, &text_bytes,
                                       error, error_size))) {
        free(text);
        goto cleanup;
    }
    text[text_bytes] = 0;
    if (conversation) {
        size_t generated_committed = generated_count ? generated_count - 1 : 0;
        size_t committed_count = prompt_count + generated_committed;
        if (conversation_has_replica(conversation, route_snapshot,
                                     context, max_rows) &&
            conversation_checkpoint(conversation, committed_count))
            fprintf(stderr, "[lumabri] Segment checkpoint unavailable; "
                            "failover will replay from token %zu\n",
                    conversation->checkpoint_count);
        if (committed_count > SIZE_MAX / sizeof *prompt_tokens) {
            free(text); goto cleanup;
        }
        int32_t *committed = malloc(committed_count * sizeof *committed);
        if (!committed) {
            free(text);
            snprintf(error, error_size, "out of memory retaining Segment KV "
                     "token prefix");
            goto cleanup;
        }
        memcpy(committed, prompt_tokens, prompt_count * sizeof *committed);
        if (generated_committed)
            memcpy(committed + prompt_count, generated,
                   generated_committed * sizeof *committed);
        free(conversation->committed_tokens);
        conversation->committed_tokens = committed;
        conversation->committed_count = committed_count;
    }
    result->text = text;
    result->text_bytes = text_bytes;
    result->tokens = generated;
    result->token_count = generated_count;
    result->prompt_count = prompt_count;
    result->elapsed_seconds = monotonic_seconds() - started;
    generated = NULL;
    rc = 0;

cleanup:
    if (rc && error && error_size && !error[0])
        snprintf(error, error_size, "Segment generation failed and the engine "
                 "reported no reason");
    if (conversation) {
        if (rc) conversation_reset(conversation);
    } else {
        for (size_t index = 0; index < opened; index++)
            remote_close(&active_chain[index]);
    }
    free(prompt_tokens);
    free(generated);
    free(logits);
    free(buffer_a);
    free(buffer_b);
    return rc;
}

static void route_print(FILE *stream, const char *prefix,
                        const LmbSegRouteSnapshot *snapshot,
                        const RemoteSegment *chain, size_t chain_count) {
    fprintf(stream, "%s Segment route generation %llu: ", prefix,
            (unsigned long long)snapshot->route_generation);
    for (size_t index = 0; index < chain_count; index++)
        fprintf(stream, "%s%s[%u:%u]%s", index ? " -> " : "",
                chain[index].route.advert.peer_name,
                chain[index].route.advert.layer_begin,
                chain[index].route.advert.layer_end,
                chain[index].route.advert.flags & LMB_SEG_ADVERT_FALLBACK
                    ? "(fallback)" : "");
    fputc('\n', stream);
    if (getenv("LUMABRI_SEGMENT_DEBUG_ROUTES"))
        for (uint32_t index = 0; index < snapshot->count; index++) {
            const LmbSegRouteEntry *entry = &snapshot->entries[index];
            fprintf(stream, "[lumabri] Segment candidate %s[%u:%u] "
                    "flags=%u load=%u/%u transport=%u context=%u rows=%u "
                    "numeric=%s\n",
                    entry->advert.peer_name, entry->advert.layer_begin,
                    entry->advert.layer_end, entry->advert.flags,
                    entry->advert.queue_depth, entry->advert.inflight,
                    entry->transport, entry->advert.max_context,
                    entry->advert.max_rows, entry->advert.numeric_class);
        }
    fflush(stream);
}

static int read_exact_stdin(void *output, size_t bytes) {
    unsigned char *cursor = output;
    while (bytes) {
        size_t got = fread(cursor, 1, bytes, stdin);
        if (!got) return -1;
        cursor += got;
        bytes -= got;
    }
    return 0;
}

static int segment_serve_loop(ColiEdgeEngine *edge,
                              const ColiEdgeCapabilities *cap,
                              LmbSegDiscovery *discovery,
                              const uint8_t model_root[32],
                              const uint8_t tokenizer_root[32],
                              uint32_t context, uint32_t max_rows,
                              uint64_t seed) {
    printf(SEGMENT_FRAME_READY "\nSTAT 0 0 0 0\n");
    fflush(stdout);
    LmbSampler sampler;
    lmb_sampler_init(&sampler, seed);
    SegmentConversation conversation;
    memset(&conversation, 0, sizeof conversation);
    char header[512];
    while (fgets(header, sizeof header, stdin)) {
        unsigned request_id = 0, slot = 0, max_tokens = 0;
        size_t prompt_bytes = 0;
        double temperature = 0.0, top_p = 0.0;
        char trailing = 0;
        if (sscanf(header, "SUBMIT %u %u %zu %u %lf %lf %c",
                   &request_id, &slot, &prompt_bytes, &max_tokens,
                   &temperature, &top_p, &trailing) != 6 ||
            !max_tokens || max_tokens > 4096 || prompt_bytes > (64u << 20) ||
            !isfinite(temperature) || temperature < 0.0 || temperature > 100.0 ||
            !isfinite(top_p) || top_p <= 0.0 || top_p > 1.0) {
            printf("ERROR %u invalid Segment SUBMIT\n", request_id);
            fflush(stdout);
            continue;
        }
        (void)slot;
        char *prompt = malloc(prompt_bytes + 1);
        if (!prompt || read_exact_stdin(prompt, prompt_bytes)) {
            free(prompt); conversation_reset(&conversation); return 1;
        }
        prompt[prompt_bytes] = 0;
        int terminator = fgetc(stdin);
        if (terminator != '\n') {
            free(prompt); conversation_reset(&conversation); return 1;
        }
        printf("ACCEPT %u\n", request_id);
        fflush(stdout);

        LmbSegRouteSnapshot snapshot;
        int have = lmb_seg_discovery_snapshot(discovery, &snapshot);
        RemoteSegment chain[LMB_SEG_ROUTE_MAX];
        size_t chain_count = 0;
        char error[256] = {0};
        GenerationResult result;
        int bad = have <= 0 || !snapshot.complete ||
                  select_chain(&snapshot, cap->num_layers, context, max_rows,
                               chain, &chain_count);
        if (bad)
            snprintf(error, sizeof error,
                     "no complete compatible Segment chain");
        if (!bad) {
            if (conversation.active && conversation.route_generation !=
                                       snapshot.route_generation)
                route_print(stderr, "[segment-route]", &snapshot,
                            chain, chain_count);
            bad = segment_generate(edge, cap, chain, chain_count,
                                   model_root, tokenizer_root,
                                   context, max_rows, prompt, prompt_bytes,
                                   NULL, 0, max_tokens,
                                   temperature, top_p, &sampler, &conversation,
                                   &snapshot,
                                   snapshot.route_generation, &result,
                                   error, sizeof error);
        }
        free(prompt);
        if (bad) {
            printf("ERROR %u %s\n", request_id, error);
            fflush(stdout);
            continue;
        }
        printf("DATA %u %zu\n", request_id, result.text_bytes);
        if (result.text_bytes)
            fwrite(result.text, 1, result.text_bytes, stdout);
        fputc('\n', stdout);
        double rate = result.elapsed_seconds > 0.0
            ? (double)result.token_count / result.elapsed_seconds : 0.0;
        printf("DONE %u STAT %zu %.3f 0 0 %zu 0\n", request_id,
               result.token_count, rate, result.prompt_count);
        fflush(stdout);
        generation_result_free(&result);
    }
    conversation_reset(&conversation);
    return 0;
}

int main(int argc, char **argv) {
    /* lmb_random shares the header-only signing implementation; retain the
     * primitive in warning-clean builds even though the chatter does not sign. */
    (void)lmb_sign;
    const char *engine_id = arg_value(argc, argv, "--engine");
    const char *model_dir = arg_value(argc, argv, "--model-dir");
    const char *model = arg_value(argc, argv, "--model");
    const char *tracker = arg_value(argc, argv, "--tracker");
    const char *model_root_text = arg_value(argc, argv, "--model-root");
    const char *tokenizer_root_text = arg_value(argc, argv, "--tokenizer-root");
    const char *prompt = arg_value(argc, argv, "--prompt");
    const char *prompt_ids_text = arg_value(argc, argv, "--prompt-ids");
    const char *expect_ids_text = arg_value(argc, argv, "--expect-ids");
    int serve_mode = has_arg(argc, argv, "--serve");
    segment_tracker = tracker;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--retry-first-run")) retry_first_run = 1;
    if (!engine_id || !model_dir || !model || !tracker ||
        (serve_mode ? (prompt || prompt_ids_text || expect_ids_text)
                    : (!!prompt == !!prompt_ids_text))) {
        usage(argv[0]); return 2;
    }
    if (strlen(engine_id) >= LMB_SEG_ENGINE_MAX ||
        strlen(model) >= LMB_SEG_MODEL_MAX || strlen(tracker) >= 256) {
        usage(argv[0]); return 2;
    }
    uint8_t model_root[32], tokenizer_root[32];
    uint32_t wanted_tokens = 3;
    const char *value = arg_value(argc, argv, "--tokens");
    if (value && lmb_parse_u32(value, 1, 4096, &wanted_tokens)) {
        usage(argv[0]); return 2;
    }
    double temperature = 0.0, top_p = 0.95;
    if (((value = arg_value(argc, argv, "--temperature")) &&
         parse_double(value, 0.0, 100.0, &temperature)) ||
        ((value = arg_value(argc, argv, "--top-p")) &&
         parse_double(value, 0.000001, 1.0, &top_p)) ||
        (arg_value(argc, argv, "--seed") &&
         parse_u64(arg_value(argc, argv, "--seed"), &(uint64_t){0}))) {
        usage(argv[0]); return 2;
    }
    uint64_t seed = sampler_seed(arg_value(argc, argv, "--seed"));
    LmbSampler sampler;
    lmb_sampler_init(&sampler, seed);
    int32_t *expected = NULL;
    size_t expected_count = 0;
    if (expect_ids_text &&
        (parse_ids(expect_ids_text, &expected, &expected_count) ||
         expected_count != wanted_tokens)) {
        fprintf(stderr, "--expect-ids must contain exactly --tokens IDs\n");
        return 2;
    }
    if (lmb_secure_init()) return 1;
    if (model_identity_resolve(tracker, model, model_root_text,
                               tokenizer_root_text, model_root,
                               tokenizer_root)) {
        fprintf(stderr, "cannot resolve a trusted model identity for %s\n",
                model);
        return 1;
    }
    if (lmb_colibri_register_all()) {
        fprintf(stderr, "cannot register all six Colibri adapters\n"); return 1;
    }
    ColiEdgeEngineOptions edge_options = {
        .struct_size = sizeof edge_options,
        .model_dir = model_dir,
    };
    ColiEdgeEngine *edge = NULL;
    /* Never hand an uninitialised buffer to the engine ABI: a failure that
     * writes nothing would otherwise be reported as whatever was on the
     * stack. */
    char error[256] = "";
    if (coli_edge_engine_open(engine_id, &edge_options, &edge,
                              error, sizeof error)) {
        fprintf(stderr, "cannot open Colibri Edge engine: %s\n",
                engine_error(error));
        return 1;
    }
    ColiEdgeCapabilities cap = { .struct_size = sizeof cap };
    error[0] = 0;
    if (coli_edge_engine_capabilities(edge, &cap, error, sizeof error)) {
        fprintf(stderr, "cannot read Edge capabilities: %s\n",
                engine_error(error));
        return 1;
    }
    uint32_t context = cap.max_context_tokens < 4096 ? cap.max_context_tokens : 4096;
    uint32_t max_rows = cap.max_batch_rows < 64 ? cap.max_batch_rows : 64;
    uint32_t discovery_timeout_ms = serve_mode ? 2500u : 15000u;
    if (((value = arg_value(argc, argv, "--context")) &&
         lmb_parse_u32(value, 1, cap.max_context_tokens, &context)) ||
        ((value = arg_value(argc, argv, "--max-rows")) &&
         lmb_parse_u32(value, 1, cap.max_batch_rows, &max_rows)) ||
        ((value = arg_value(argc, argv, "--discovery-timeout-ms")) &&
         lmb_parse_u32(value, 250, 60000, &discovery_timeout_ms))) {
        usage(argv[0]); return 2;
    }
    LmbSegQuery query;
    memset(&query, 0, sizeof query);
    snprintf(query.model, sizeof query.model, "%s", model);
    memcpy(query.model_root, model_root, sizeof model_root);
    memcpy(query.tokenizer_root, tokenizer_root, sizeof tokenizer_root);
    query.layer_end = cap.num_layers;
    /* Ask discovery for capability truth, not for the caller's preferred
     * context. Selection below first tries the preference and only then
     * negotiates down to the largest complete executor chain. */
    query.context_tokens = 1;
    query.rows = max_rows;
    query.state_dtype = cap.state_dtype;
    query.state_width = cap.state_width;
    query.required_capabilities = LMB_SEG_CAP_RANGE_NATIVE |
                                  LMB_SEG_CAP_MULTI_SESSION |
                                  LMB_SEG_CAP_SNAPSHOT;
    snprintf(query.engine_id, sizeof query.engine_id, "%s", cap.engine_id);
    snprintf(query.state_schema, sizeof query.state_schema, "%s", cap.state_schema);
    snprintf(query.numeric_class, sizeof query.numeric_class, "%s", cap.numeric_class);
    LmbSegDiscovery *discovery = lmb_seg_discovery_start(tracker, &query, 250);
    if (!discovery) { fprintf(stderr, "cannot start Segment discovery\n"); return 1; }
    LmbSegRouteSnapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    int have = 0, fetched = 0;
    uint32_t discovery_attempts = (discovery_timeout_ms + 249u) / 250u;
    for (uint32_t i = 0; i < discovery_attempts && !have; i++) {
        sleep_ms(250);
        have = lmb_seg_discovery_snapshot(discovery, &snapshot);
        if (have > 0) fetched = 1;
        if (have > 0 && !snapshot.complete) have = 0;
    }
    if (!have) {
        fprintf(stderr, "no complete compatible Segment chain after %.2f seconds "
                        "(snapshot=%s, compatible peers=%u, engine=%s, "
                        "schema=%s, numeric=%s, dtype=%u, width=%u, "
                        "layers=0:%u, rows=%u, context=%u)\n",
                (double)discovery_timeout_ms / 1000.0,
                fetched ? "yes" : "no", fetched ? snapshot.count : 0,
                query.engine_id, query.state_schema, query.numeric_class,
                query.state_dtype, query.state_width, query.layer_end,
                query.rows, query.context_tokens);
        lmb_seg_discovery_stop(discovery); return 1;
    }
    RemoteSegment chain[LMB_SEG_ROUTE_MAX];
    size_t chain_count = 0;
    uint32_t requested_context = context;
    if (select_chain(&snapshot, cap.num_layers, context, max_rows,
                     chain, &chain_count)) {
        uint32_t ceiling = context;
        int selected = 0;
        while (ceiling > 1 && !selected) {
            uint32_t next = 0;
            for (uint32_t i = 0; i < snapshot.count; i++) {
                uint32_t available = snapshot.entries[i].advert.max_context;
                if (available < ceiling && available > next) next = available;
            }
            if (!next) break;
            ceiling = next;
            if (!select_chain(&snapshot, cap.num_layers, ceiling, max_rows,
                              chain, &chain_count)) {
                context = ceiling;
                selected = 1;
            }
        }
        if (!selected) {
            fprintf(stderr, "tracker coverage cannot form an "
                            "executor-aligned chain\n");
            lmb_seg_discovery_stop(discovery); return 1;
        }
    }
    for (size_t i = 0; i < chain_count; i++)
        if (chain[i].route.advert.max_context < context)
            context = chain[i].route.advert.max_context;
    if (context < requested_context) {
        fprintf(stderr, "[lumabri] Segment context negotiated to %u tokens "
                        "(requested %u; executor capability)\n",
                context, requested_context);
    }
    route_print(serve_mode ? stderr : stdout,
                serve_mode ? "[segment-route]" : "[lumabri]",
                &snapshot, chain, chain_count);
    if (serve_mode) {
        int result = segment_serve_loop(edge, &cap, discovery,
                                        model_root, tokenizer_root,
                                        context, max_rows, seed);
        lmb_seg_discovery_stop(discovery);
        coli_edge_engine_close(edge);
        free(expected);
        return result;
    }

    size_t prompt_count = 0;
    int32_t *prompt_tokens = NULL;
    if (prompt_ids_text && parse_ids(prompt_ids_text, &prompt_tokens,
                                     &prompt_count)) {
        fprintf(stderr, "invalid --prompt-ids list\n");
        lmb_seg_discovery_stop(discovery);
        coli_edge_engine_close(edge);
        free(expected);
        return 2;
    }
    GenerationResult generated;
    int bad = segment_generate(edge, &cap, chain, chain_count,
                               model_root, tokenizer_root,
                               context, max_rows,
                               prompt, prompt ? strlen(prompt) : 0,
                               prompt_tokens, prompt_count, wanted_tokens,
                               temperature, top_p, &sampler, NULL, &snapshot, 0,
                               &generated, error, sizeof error);
    free(prompt_tokens);
    if (bad) {
        fprintf(stderr, "%s\n", error[0] ? error : "Segment generation failed");
        lmb_seg_discovery_stop(discovery);
        coli_edge_engine_close(edge);
        free(expected);
        return 1;
    }
    printf("%s\n", generated.text);
    printf("[lumabri] token-ids:");
    for (size_t i = 0; i < generated.token_count; i++)
        printf("%s%d", i ? "," : " ", generated.tokens[i]);
    printf("\n");
    if (expected && (generated.token_count != expected_count ||
        memcmp(generated.tokens, expected,
               expected_count * sizeof *expected))) {
        fprintf(stderr, "generated token IDs differ from independent oracle\n");
        generation_result_free(&generated);
        lmb_seg_discovery_stop(discovery);
        coli_edge_engine_close(edge);
        free(expected);
        return 1;
    }
    generation_result_free(&generated);
    lmb_seg_discovery_stop(discovery);
    coli_edge_engine_close(edge);
    free(expected);
    return 0;
}
