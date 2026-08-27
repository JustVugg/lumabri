#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "lumabri_proto.h"
#include "lumabri_segment.h"
#include "lumabri_segment_discovery.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
#include "segment_colibri.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    LmbSegRouteEntry route;
    int fd;
    LmbSegOpen open;
    uint64_t sequence;
    uint64_t position;
} RemoteSegment;

static int retry_first_run;

static void sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000u),
                           (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
}

static const char *arg_value(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static void usage(const char *program) {
    fprintf(stderr,
        "usage: %s --engine ID --model-dir DIR --model NAME "
        "--tracker HOST:PORT --model-root HEX64 --tokenizer-root HEX64 "
        "(--prompt TEXT | --prompt-ids CSV) [--expect-ids CSV] "
        "[--tokens N] [--context N] [--max-rows N] [--retry-first-run]\n",
        program);
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
                        RemoteSegment *chain, size_t *chain_count) {
    unsigned viable[LMB_SEG_ROUTE_MAX] = {0};
    for (uint32_t pass = 0; pass < snapshot->count; pass++) {
        int changed = 0;
        for (uint32_t i = 0; i < snapshot->count; i++) {
            const LmbSegRouteEntry *entry = &snapshot->entries[i];
            if (viable[i] || !(entry->transport & LMB_SEG_TRANSPORT_DIRECT) ||
                entry->advert.layer_begin >= entry->advert.layer_end ||
                entry->advert.layer_end > layers) continue;
            int reaches_end = entry->advert.layer_end == layers;
            for (uint32_t j = 0; j < snapshot->count && !reaches_end; j++)
                reaches_end = viable[j] &&
                    snapshot->entries[j].advert.layer_begin ==
                    entry->advert.layer_end;
            if (reaches_end) { viable[i] = 1; changed = 1; }
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
            if (!best || candidate->advert.layer_end > best->advert.layer_end ||
                (candidate->advert.layer_end == best->advert.layer_end &&
                 candidate->advert.queue_depth + candidate->advert.inflight <
                 best->advert.queue_depth + best->advert.inflight))
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

static int remote_open(RemoteSegment *remote, const LmbSegId *session_id,
                       const uint8_t model_root[32],
                       const uint8_t tokenizer_root[32], uint32_t context,
                       uint32_t max_rows) {
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
    open->ttl_ms = 60000;
    open->capabilities = advert->capabilities;
    snprintf(open->engine_id, sizeof open->engine_id, "%s", advert->engine_id);
    snprintf(open->state_schema, sizeof open->state_schema, "%s",
             advert->state_schema);
    snprintf(open->numeric_class, sizeof open->numeric_class, "%s",
             advert->numeric_class);
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (lmb_seg_open_encode(open, &body, &body_len)) return -1;
    remote->fd = lmb_connect(advert->addr);
    if (remote->fd < 0 ||
        lmb_send(remote->fd, LMB_SEG_OPEN, body, body_len, NULL, 0)) {
        free(body); return -1;
    }
    free(body);
    LmbMsg msg = {0};
    LmbSegReply reply;
    int bad = lmb_recv(remote->fd, &msg) ||
              reply_ok(&msg, LMB_SEG_OPEN_R, session_id, &open->request_id,
                       open->owner.route_generation, &reply) || msg.pay_len;
    lmb_msg_free(&msg);
    if (bad) return -1;
    remote->sequence = reply.next_sequence;
    remote->position = reply.next_position;
    return 0;
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
    int bad = lmb_send(remote->fd, LMB_SEG_RUN, body, body_len,
                       input, (uint32_t)bytes);
    if (bad) { free(body); return -1; }
    LmbMsg msg = {0};
    LmbSegReply reply;
    bad = lmb_recv(remote->fd, &msg) ||
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
        bad = lmb_send(remote->fd, LMB_SEG_RUN, body, body_len,
                       input, (uint32_t)bytes);
        memset(&msg, 0, sizeof msg);
        LmbSegReply duplicate;
        if (!bad) bad = lmb_recv(remote->fd, &msg) ||
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

static void remote_close(RemoteSegment *remote) {
    if (remote->fd < 0) return;
    LmbSegControl control;
    memset(&control, 0, sizeof control);
    control.session_id = remote->open.session_id;
    lmb_random(control.request_id.bytes, sizeof control.request_id.bytes);
    control.owner = remote->open.owner;
    control.sequence = remote->sequence;
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (!lmb_seg_control_encode(&control, &body, &body_len)) {
        (void)lmb_send(remote->fd, LMB_SEG_CLOSE, body, body_len, NULL, 0);
        LmbMsg reply = {0};
        if (!lmb_recv(remote->fd, &reply)) lmb_msg_free(&reply);
    }
    free(body);
    lmb_close(remote->fd);
    remote->fd = -1;
}

static int chain_run(RemoteSegment *chain, size_t count,
                     const int32_t *tokens, uint32_t rows,
                     uint8_t **first, uint8_t **second, size_t bytes) {
    uint8_t *input = *first, *output = *second;
    for (size_t i = 0; i < count; i++) {
        if (remote_run(&chain[i], tokens, rows, input, bytes, output)) return -1;
        uint8_t *swap = input; input = output; output = swap;
    }
    *first = input;
    *second = output;
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
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--retry-first-run")) retry_first_run = 1;
    if (!engine_id || !model_dir || !model || !tracker || !model_root_text ||
        !tokenizer_root_text || (!!prompt == !!prompt_ids_text)) {
        usage(argv[0]); return 2;
    }
    if (strlen(engine_id) >= LMB_SEG_ENGINE_MAX ||
        strlen(model) >= LMB_SEG_MODEL_MAX || strlen(tracker) >= 256) {
        usage(argv[0]); return 2;
    }
    uint8_t model_root[32], tokenizer_root[32];
    if (lmb_hex_root(model_root_text, model_root) ||
        lmb_hex_root(tokenizer_root_text, tokenizer_root)) {
        fprintf(stderr, "roots must be non-zero 64-character hex values\n");
        return 2;
    }
    uint32_t wanted_tokens = 3;
    const char *value = arg_value(argc, argv, "--tokens");
    if (value && lmb_parse_u32(value, 1, 4096, &wanted_tokens)) {
        usage(argv[0]); return 2;
    }
    int32_t *expected = NULL;
    size_t expected_count = 0;
    if (expect_ids_text &&
        (parse_ids(expect_ids_text, &expected, &expected_count) ||
         expected_count != wanted_tokens)) {
        fprintf(stderr, "--expect-ids must contain exactly --tokens IDs\n");
        return 2;
    }
    if (lmb_secure_init()) return 1;
    if (lmb_colibri_register_all()) {
        fprintf(stderr, "cannot register all six Colibri adapters\n"); return 1;
    }
    ColiEdgeEngineOptions edge_options = {
        .struct_size = sizeof edge_options,
        .model_dir = model_dir,
    };
    ColiEdgeEngine *edge = NULL;
    char error[256];
    if (coli_edge_engine_open(engine_id, &edge_options, &edge,
                              error, sizeof error)) {
        fprintf(stderr, "cannot open Colibri Edge engine: %s\n", error);
        return 1;
    }
    ColiEdgeCapabilities cap = { .struct_size = sizeof cap };
    if (coli_edge_engine_capabilities(edge, &cap, error, sizeof error)) {
        fprintf(stderr, "cannot read Edge capabilities: %s\n", error);
        return 1;
    }
    uint32_t context = cap.max_context_tokens < 4096 ? cap.max_context_tokens : 4096;
    uint32_t max_rows = cap.max_batch_rows < 64 ? cap.max_batch_rows : 64;
    if (((value = arg_value(argc, argv, "--context")) &&
         lmb_parse_u32(value, 1, cap.max_context_tokens, &context)) ||
        ((value = arg_value(argc, argv, "--max-rows")) &&
         lmb_parse_u32(value, 1, cap.max_batch_rows, &max_rows))) {
        usage(argv[0]); return 2;
    }
    LmbSegQuery query;
    memset(&query, 0, sizeof query);
    snprintf(query.model, sizeof query.model, "%s", model);
    memcpy(query.model_root, model_root, sizeof model_root);
    memcpy(query.tokenizer_root, tokenizer_root, sizeof tokenizer_root);
    query.layer_end = cap.num_layers;
    query.context_tokens = context;
    query.rows = max_rows;
    query.state_dtype = cap.state_dtype;
    query.state_width = cap.state_width;
    query.required_capabilities = LMB_SEG_CAP_RANGE_NATIVE |
                                  LMB_SEG_CAP_MULTI_SESSION;
    snprintf(query.engine_id, sizeof query.engine_id, "%s", cap.engine_id);
    snprintf(query.state_schema, sizeof query.state_schema, "%s", cap.state_schema);
    snprintf(query.numeric_class, sizeof query.numeric_class, "%s", cap.numeric_class);
    LmbSegDiscovery *discovery = lmb_seg_discovery_start(tracker, &query, 250);
    if (!discovery) { fprintf(stderr, "cannot start Segment discovery\n"); return 1; }
    LmbSegRouteSnapshot snapshot;
    memset(&snapshot, 0, sizeof snapshot);
    int have = 0, fetched = 0;
    for (int i = 0; i < 60 && !have; i++) {
        sleep_ms(250);
        have = lmb_seg_discovery_snapshot(discovery, &snapshot);
        if (have > 0) fetched = 1;
        if (have > 0 && !snapshot.complete) have = 0;
    }
    if (!have) {
        fprintf(stderr, "no complete compatible Segment chain after 15 seconds "
                        "(snapshot=%s, compatible peers=%u, engine=%s, "
                        "schema=%s, numeric=%s, dtype=%u, width=%u, "
                        "layers=0:%u, rows=%u, context=%u)\n",
                fetched ? "yes" : "no", fetched ? snapshot.count : 0,
                query.engine_id, query.state_schema, query.numeric_class,
                query.state_dtype, query.state_width, query.layer_end,
                query.rows, query.context_tokens);
        lmb_seg_discovery_stop(discovery); return 1;
    }
    RemoteSegment chain[LMB_SEG_ROUTE_MAX];
    size_t chain_count = 0;
    if (select_chain(&snapshot, cap.num_layers, chain, &chain_count)) {
        fprintf(stderr, "tracker coverage cannot form an executor-aligned chain\n");
        lmb_seg_discovery_stop(discovery); return 1;
    }
    LmbSegId session_id;
    lmb_random(session_id.bytes, sizeof session_id.bytes);
    for (size_t i = 0; i < chain_count; i++) {
        if (remote_open(&chain[i], &session_id, model_root, tokenizer_root,
                        context, max_rows)) {
            fprintf(stderr, "cannot open segment %s at %s\n",
                    chain[i].route.advert.peer_name, chain[i].route.advert.addr);
            for (size_t j = 0; j < i; j++) remote_close(&chain[j]);
            lmb_seg_discovery_stop(discovery); return 1;
        }
    }
    printf("[lumabri] route generation %llu: ",
           (unsigned long long)snapshot.route_generation);
    for (size_t i = 0; i < chain_count; i++)
        printf("%s%s[%u:%u]", i ? " -> " : "",
               chain[i].route.advert.peer_name,
               chain[i].route.advert.layer_begin,
               chain[i].route.advert.layer_end);
    printf("\n"); fflush(stdout);

    size_t prompt_count = 0;
    int32_t *prompt_tokens = NULL;
    if (prompt_ids_text) {
        if (parse_ids(prompt_ids_text, &prompt_tokens, &prompt_count)) {
            fprintf(stderr, "invalid --prompt-ids list\n"); return 2;
        }
    } else {
        if (coli_edge_tokenize(edge, prompt, strlen(prompt), NULL, 0,
                               &prompt_count, error, sizeof error) || !prompt_count) {
            fprintf(stderr, "cannot tokenize prompt: %s\n", error); return 1;
        }
        prompt_tokens = malloc(prompt_count * sizeof *prompt_tokens);
        size_t actual_count = 0;
        if (!prompt_tokens ||
            coli_edge_tokenize(edge, prompt, strlen(prompt), prompt_tokens,
                               prompt_count, &actual_count, error, sizeof error) ||
            actual_count != prompt_count) {
            fprintf(stderr, "cannot tokenize prompt: %s\n", error); return 1;
        }
    }
    if (prompt_count + wanted_tokens > context) {
        fprintf(stderr, "prompt plus output exceeds context (%u)\n", context);
        return 1;
    }
    int32_t *generated = malloc(wanted_tokens * sizeof *generated);
    size_t max_bytes = lmb_state_bytes(max_rows, cap.state_width, cap.state_dtype);
    uint8_t *buffer_a = max_bytes ? malloc(max_bytes) : NULL;
    uint8_t *buffer_b = max_bytes ? malloc(max_bytes) : NULL;
    if (!generated || !buffer_a || !buffer_b) return 1;
    uint8_t *final_state = NULL;
    uint32_t final_rows = 0;
    for (size_t offset = 0; offset < prompt_count; offset += max_rows) {
        uint32_t rows = (uint32_t)(prompt_count - offset);
        if (rows > max_rows) rows = max_rows;
        size_t bytes = lmb_state_bytes(rows, cap.state_width, cap.state_dtype);
        ColiEdgeEmbedRequest embed = {
            .struct_size = sizeof embed, .rows = rows,
            .token_ids = prompt_tokens + offset, .token_count = rows,
            .output = buffer_a, .output_bytes = bytes,
        };
        if (coli_edge_embed(edge, &embed, error, sizeof error)) {
            fprintf(stderr, "embedding failed: %s\n", error); return 1;
        }
        uint8_t *first = buffer_a, *second = buffer_b;
        if (chain_run(chain, chain_count, prompt_tokens + offset, rows,
                      &first, &second, bytes)) {
            fprintf(stderr, "Segment peer failed; session ended because checkpoint/replay is not available yet\n");
            return 1;
        }
        final_state = first; final_rows = rows;
        if (first != buffer_a) { uint8_t *swap = buffer_a; buffer_a = buffer_b; buffer_b = swap; }
    }
    size_t element = cap.state_dtype == COLI_EDGE_DTYPE_F32 ? 4u : 2u;
    const uint8_t *last = final_state + (size_t)(final_rows - 1) * cap.state_width * element;
    ColiEdgeSelectRequest select = {
        .struct_size = sizeof select, .rows = 1,
        .input = last, .input_bytes = (size_t)cap.state_width * element,
        .token_ids = generated, .token_capacity = 1,
    };
    if (coli_edge_select(edge, &select, error, sizeof error)) {
        fprintf(stderr, "token selection failed: %s\n", error); return 1;
    }
    size_t generated_count = 1;
    while (generated_count < wanted_tokens &&
           generated[generated_count - 1] != cap.eos_token_id) {
        int32_t token = generated[generated_count - 1];
        size_t bytes = lmb_state_bytes(1, cap.state_width, cap.state_dtype);
        ColiEdgeEmbedRequest embed = {
            .struct_size = sizeof embed, .rows = 1,
            .token_ids = &token, .token_count = 1,
            .output = buffer_a, .output_bytes = bytes,
        };
        if (coli_edge_embed(edge, &embed, error, sizeof error)) {
            fprintf(stderr, "embedding failed: %s\n", error); return 1;
        }
        uint8_t *first = buffer_a, *second = buffer_b;
        if (chain_run(chain, chain_count, &token, 1, &first, &second, bytes)) {
            fprintf(stderr, "Segment peer failed; session ended because checkpoint/replay is not available yet\n");
            return 1;
        }
        select.input = first;
        select.token_ids = generated + generated_count;
        if (coli_edge_select(edge, &select, error, sizeof error)) {
            fprintf(stderr, "token selection failed: %s\n", error); return 1;
        }
        generated_count++;
    }
    size_t text_bytes = 0;
    if (coli_edge_detokenize(edge, generated, generated_count, NULL, 0,
                             &text_bytes, error, sizeof error)) {
        fprintf(stderr, "detokenize sizing failed: %s\n", error); return 1;
    }
    char *text = malloc(text_bytes + 1);
    if (!text || coli_edge_detokenize(edge, generated, generated_count,
                                      text, text_bytes + 1, &text_bytes,
                                      error, sizeof error)) {
        fprintf(stderr, "detokenize failed: %s\n", error); return 1;
    }
    text[text_bytes] = 0;
    printf("%s\n", text);
    printf("[lumabri] token-ids:");
    for (size_t i = 0; i < generated_count; i++) printf("%s%d", i ? "," : " ", generated[i]);
    printf("\n");
    if (expected && (generated_count != expected_count ||
        memcmp(generated, expected, expected_count * sizeof *expected))) {
        fprintf(stderr, "generated token IDs differ from independent oracle\n");
        return 1;
    }
    for (size_t i = 0; i < chain_count; i++) remote_close(&chain[i]);
    lmb_seg_discovery_stop(discovery);
    coli_edge_engine_close(edge);
    free(text); free(prompt_tokens); free(generated); free(expected);
    free(buffer_a); free(buffer_b);
    return 0;
}
