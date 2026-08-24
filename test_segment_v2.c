#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "lumabri_segment.h"
#include "lumabri_proto.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *engine;
    const char *schema;
    const char *numeric;
    uint32_t dtype;
    uint32_t width;
    uint64_t extra_caps;
} TinySchema;

static const TinySchema tiny_schemas[] = {
    { "olmoe", "kv-standard-v1", "olmoe-f32-strict-tiny", LMB_SEG_DTYPE_F32, 8, 0 },
    { "glm", "mla-dsa-rope-device-v1", "glm-f16-strict-tiny", LMB_SEG_DTYPE_F16, 12, LMB_SEG_CAP_CUDA },
    { "inkling", "kv-global-sliding-conv-v1", "inkling-f32-strict-tiny", LMB_SEG_DTYPE_F32, 10, LMB_SEG_CAP_METAL },
    { "kimi_k3", "mla-kda-conv-attnres-v1", "kimi-bf16-strict-tiny", LMB_SEG_DTYPE_BF16, 14, LMB_SEG_CAP_VULKAN },
    { "qwen36", "kv-deltanet-conv-ring-v1", "qwen-f16-strict-tiny", LMB_SEG_DTYPE_F16, 16, LMB_SEG_CAP_CUDA },
    { "deepseek_v4", "mhc-window-compressor-indexer-v1", "dsv4-bf16-strict-tiny", LMB_SEG_DTYPE_BF16, 18, LMB_SEG_CAP_HIP },
};

static void fill_id(LmbSegId *id, unsigned seed) {
    for (size_t i = 0; i < sizeof id->bytes; i++)
        id->bytes[i] = (uint8_t)(seed + 17u * (unsigned)i + 1u);
}

static void fill_root(uint8_t root[LMB_SEG_ROOT_BYTES], unsigned seed) {
    for (size_t i = 0; i < LMB_SEG_ROOT_BYTES; i++)
        root[i] = (uint8_t)(seed + 29u * (unsigned)i + 1u);
}

static uint8_t *find_bytes(uint8_t *haystack, size_t haystack_len,
                           const void *needle, size_t needle_len) {
    if (!needle_len || needle_len > haystack_len) return NULL;
    for (size_t i = 0; i <= haystack_len - needle_len; i++)
        if (!memcmp(haystack + i, needle, needle_len)) return haystack + i;
    return NULL;
}

static LmbSegOpen make_open(const TinySchema *schema, unsigned seed) {
    LmbSegOpen open;
    memset(&open, 0, sizeof open);
    fill_id(&open.session_id, seed);
    fill_id(&open.request_id, seed + 1);
    fill_id(&open.owner.lease_id, seed + 2);
    open.owner.fencing_epoch = 1;
    open.owner.route_generation = 1;
    fill_root(open.model_root, seed + 3);
    fill_root(open.tokenizer_root, seed + 4);
    open.layer_begin = 2;
    open.layer_end = 5;
    open.context_tokens = 64;
    open.max_rows = 8;
    open.state_dtype = schema->dtype;
    open.state_width = schema->width;
    open.ttl_ms = 1000;
    open.capabilities = LMB_SEG_CAP_TOKEN_IDS | LMB_SEG_CAP_SNAPSHOT |
                        LMB_SEG_CAP_RANGE_NATIVE | LMB_SEG_CAP_MULTI_SESSION |
                        LMB_SEG_CAP_CPU | schema->extra_caps;
    snprintf(open.engine_id, sizeof open.engine_id, "%s", schema->engine);
    snprintf(open.state_schema, sizeof open.state_schema, "%s", schema->schema);
    snprintf(open.numeric_class, sizeof open.numeric_class, "%s", schema->numeric);
    return open;
}

static void assert_open_same(const LmbSegOpen *a, const LmbSegOpen *b) {
    assert(lmb_seg_id_equal(&a->session_id, &b->session_id));
    assert(lmb_seg_id_equal(&a->request_id, &b->request_id));
    assert(lmb_seg_id_equal(&a->owner.lease_id, &b->owner.lease_id));
    assert(a->owner.fencing_epoch == b->owner.fencing_epoch);
    assert(a->owner.route_generation == b->owner.route_generation);
    assert(!memcmp(a->model_root, b->model_root, sizeof a->model_root));
    assert(!memcmp(a->tokenizer_root, b->tokenizer_root,
                   sizeof a->tokenizer_root));
    assert(a->layer_begin == b->layer_begin && a->layer_end == b->layer_end);
    assert(a->context_tokens == b->context_tokens && a->max_rows == b->max_rows);
    assert(a->state_dtype == b->state_dtype && a->state_width == b->state_width);
    assert(a->ttl_ms == b->ttl_ms && a->capabilities == b->capabilities);
    assert(!strcmp(a->engine_id, b->engine_id));
    assert(!strcmp(a->state_schema, b->state_schema));
    assert(!strcmp(a->numeric_class, b->numeric_class));
}

static void test_open_matrix(void) {
    for (size_t i = 0; i < sizeof tiny_schemas / sizeof tiny_schemas[0]; i++) {
        LmbSegOpen open = make_open(&tiny_schemas[i], (unsigned)(10 + i * 10));
        uint8_t *body = NULL, *again = NULL;
        uint32_t body_len = 0, again_len = 0;
        assert(lmb_seg_open_valid(&open));
        assert(!lmb_seg_open_encode(&open, &body, &body_len));
        assert(body && body_len && body_len <= LMB_MAX_SMALL_BODY);
        LmbSegOpen decoded;
        assert(!lmb_seg_open_decode(body, body_len, &decoded));
        assert_open_same(&open, &decoded);
        assert(!lmb_seg_open_encode(&decoded, &again, &again_len));
        assert(body_len == again_len && !memcmp(body, again, body_len));
        for (uint32_t cut = 0; cut < body_len; cut++) {
            LmbSegOpen truncated;
            assert(lmb_seg_open_decode(body, cut, &truncated));
        }
        uint8_t saved = body[4];
        body[4] = (uint8_t)(LMB_SEG_V2_VERSION + 1);
        assert(lmb_seg_open_decode(body, body_len, &decoded));
        body[4] = saved;
        uint8_t *engine = find_bytes(body, body_len, open.engine_id,
                                     strlen(open.engine_id));
        assert(engine && strlen(open.engine_id) > 1);
        saved = engine[1];
        engine[1] = 0;
        assert(lmb_seg_open_decode(body, body_len, &decoded));
        engine[1] = saved;
        free(again);
        free(body);
    }

    LmbSegOpen invalid = make_open(&tiny_schemas[0], 90);
    invalid.capabilities &= ~LMB_SEG_CAP_MULTI_SESSION;
    assert(!lmb_seg_open_valid(&invalid));
    invalid = make_open(&tiny_schemas[0], 91);
    invalid.capabilities |= UINT64_C(1) << 63;
    assert(!lmb_seg_open_valid(&invalid));
    invalid = make_open(&tiny_schemas[0], 92);
    memset(invalid.model_root, 0, sizeof invalid.model_root);
    assert(!lmb_seg_open_valid(&invalid));
    invalid = make_open(&tiny_schemas[0], 93);
    invalid.layer_end = invalid.layer_begin;
    assert(!lmb_seg_open_valid(&invalid));
}

static LmbSegRun make_run(const LmbSegOpen *open, unsigned request_seed,
                          uint64_t sequence, uint64_t position,
                          uint32_t rows, const int32_t *tokens) {
    LmbSegRun run;
    memset(&run, 0, sizeof run);
    run.session_id = open->session_id;
    fill_id(&run.request_id, request_seed);
    run.owner = open->owner;
    run.sequence = sequence;
    run.position = position;
    run.rows = rows;
    run.token_count = rows;
    run.token_ids = tokens;
    return run;
}

static void test_run_wire(void) {
    LmbSegOpen open = make_open(&tiny_schemas[0], 100);
    int32_t tokens[3] = { 7, -1, 9001 };
    LmbSegRun run = make_run(&open, 110, 4, 12, 3, tokens);
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    assert(!lmb_seg_run_encode(&run, &body, &body_len));
    int32_t decoded_tokens[3] = {0};
    LmbSegRun decoded;
    assert(!lmb_seg_run_decode(body, body_len, &decoded,
                               decoded_tokens, 3));
    assert(lmb_seg_id_equal(&run.session_id, &decoded.session_id));
    assert(lmb_seg_id_equal(&run.request_id, &decoded.request_id));
    assert(decoded.sequence == 4 && decoded.position == 12 && decoded.rows == 3);
    assert(decoded.token_count == 3 && !memcmp(tokens, decoded_tokens, sizeof tokens));
    assert(lmb_seg_run_decode(body, body_len, &decoded, decoded_tokens, 2));
    for (uint32_t cut = 0; cut < body_len; cut++)
        assert(lmb_seg_run_decode(body, cut, &decoded, decoded_tokens, 3));
    size_t payload_bytes = 0;
    assert(!lmb_seg_run_payload_bytes(&open, &run, &payload_bytes));
    assert(payload_bytes == (size_t)3 * open.state_width * sizeof(float));
    run.position = open.context_tokens - 2;
    assert(lmb_seg_run_payload_bytes(&open, &run, &payload_bytes));
    run.position = 12;
    run.token_count = 0;
    run.token_ids = NULL;
    assert(lmb_seg_run_payload_bytes(&open, &run, &payload_bytes));
    run.token_count = run.rows;
    run.token_ids = tokens;
    run.sequence = UINT64_MAX;
    assert(lmb_seg_run_encode(&run, &body, &body_len));
    free(body);
}

static void test_control_wire(void) {
    LmbSegOpen open = make_open(&tiny_schemas[1], 120);
    LmbSegTransfer transfer;
    memset(&transfer, 0, sizeof transfer);
    transfer.session_id = open.session_id;
    fill_id(&transfer.request_id, 130);
    transfer.owner = open.owner;
    transfer.sequence = 9;
    transfer.position = 27;
    transfer.snapshot_size = 100;
    transfer.offset = 0;
    transfer.chunk_len = 60;
    transfer.flags = LMB_SEG_XFER_BEGIN;
    uint8_t *body = NULL;
    uint32_t body_len = 0;
    assert(!lmb_seg_transfer_encode(&transfer, &body, &body_len));
    LmbSegTransfer transfer_out;
    assert(!lmb_seg_transfer_decode(body, body_len, &transfer_out));
    assert(transfer_out.sequence == 9 && transfer_out.position == 27);
    assert(transfer_out.snapshot_size == 100 && transfer_out.chunk_len == 60);
    free(body);

    LmbSegControl control = {
        .session_id = open.session_id,
        .owner = open.owner,
        .sequence = 9,
    };
    fill_id(&control.request_id, 131);
    assert(!lmb_seg_control_encode(&control, &body, &body_len));
    LmbSegControl control_out;
    assert(!lmb_seg_control_decode(body, body_len, &control_out));
    assert(control_out.sequence == 9);
    free(body);

    LmbSegReply reply = {
        .session_id = open.session_id,
        .request_id = control.request_id,
        .status = LMB_SEG_STATUS_NEEDS_RESTORE,
        .flags = 3,
        .next_sequence = 9,
        .next_position = 27,
        .fencing_epoch = 4,
        .route_generation = 6,
    };
    assert(!lmb_seg_reply_encode(&reply, &body, &body_len));
    LmbSegReply reply_out;
    assert(!lmb_seg_reply_decode(body, body_len, &reply_out));
    assert(reply_out.status == LMB_SEG_STATUS_NEEDS_RESTORE);
    assert(reply_out.next_sequence == 9 && reply_out.next_position == 27);
    free(body);

    LmbSegTransferReply chunk_reply = {
        .reply = {
            .session_id = open.session_id,
            .request_id = control.request_id,
            .status = LMB_SEG_STATUS_OK,
            .next_sequence = 9,
            .next_position = 27,
            .fencing_epoch = 1,
            .route_generation = 1,
        },
        .snapshot_size = 100,
        .offset = 60,
        .chunk_len = 40,
        .transfer_flags = LMB_SEG_XFER_END,
    };
    assert(!lmb_seg_transfer_reply_encode(&chunk_reply, &body, &body_len));
    LmbSegTransferReply chunk_out;
    assert(!lmb_seg_transfer_reply_decode(body, body_len, &chunk_out));
    assert(chunk_out.reply.status == LMB_SEG_STATUS_OK);
    assert(chunk_out.snapshot_size == 100 && chunk_out.offset == 60);
    assert(chunk_out.chunk_len == 40 &&
           chunk_out.transfer_flags == LMB_SEG_XFER_END);
    free(body);

    assert(lmb_frame_shape_ok(LMB_SEG_OPEN, 32, 0));
    assert(!lmb_frame_shape_ok(LMB_SEG_OPEN, 32, 1));
    assert(lmb_frame_shape_ok(LMB_SEG_RUN, 32, LMB_MAX_PAY));
    assert(lmb_frame_shape_ok(LMB_SEG_SNAPSHOT_R, 32, LMB_MAX_PAY));
    assert(lmb_frame_shape_ok(LMB_SEG_RESTORE, 32, LMB_MAX_PAY));
}

static void test_session_table(void) {
    LmbSegOpen first = make_open(&tiny_schemas[0], 140);
    LmbSegOpen second = make_open(&tiny_schemas[5], 150);
    LmbSegOpen third = make_open(&tiny_schemas[3], 160);
    LmbSegTable *table = lmb_seg_table_create(2);
    assert(table);
    assert(lmb_seg_table_open(table, &first, 100) == LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_open(table, &first, 101) == LMB_SEG_STATUS_DUPLICATE);
    assert(lmb_seg_table_open(table, &second, 100) == LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_open(table, &third, 100) == LMB_SEG_STATUS_QUOTA);

    int32_t tokens[2] = { 11, 12 };
    LmbSegRun run = make_run(&first, 170, 0, 0, 2, tokens);
    size_t payload_bytes = 0;
    assert(!lmb_seg_run_payload_bytes(&first, &run, &payload_bytes));
    uint8_t *payload = (uint8_t *)malloc(payload_bytes);
    assert(payload);
    for (size_t i = 0; i < payload_bytes; i++) payload[i] = (uint8_t)(i * 3u);

    assert(lmb_seg_table_run_begin(table, &run, payload, payload_bytes, 110) ==
           LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_run_begin(table, &run, payload, payload_bytes, 110) ==
           LMB_SEG_STATUS_BUSY);
    assert(lmb_seg_table_run_commit(table, &run, 111) == LMB_SEG_STATUS_OK);

    uint64_t sequence = 99, position = 99;
    int needs_restore = 1;
    assert(lmb_seg_table_state(table, &first.session_id,
                               &sequence, &position, &needs_restore) ==
           LMB_SEG_STATUS_OK);
    assert(sequence == 1 && position == 2 && !needs_restore);
    assert(lmb_seg_table_state(table, &second.session_id,
                               &sequence, &position, &needs_restore) ==
           LMB_SEG_STATUS_OK);
    assert(sequence == 0 && position == 0 && !needs_restore);

    assert(lmb_seg_table_run_begin(table, &run, payload, payload_bytes, 112) ==
           LMB_SEG_STATUS_DUPLICATE);
    payload[0] ^= 1;
    assert(lmb_seg_table_run_begin(table, &run, payload, payload_bytes, 112) ==
           LMB_SEG_STATUS_CONFLICT);
    payload[0] ^= 1;

    LmbSegRun next = make_run(&first, 171, 1, 2, 2, tokens);
    LmbSegRun future = make_run(&first, 172, 3, 2, 2, tokens);
    assert(lmb_seg_table_run_begin(table, &future, payload, payload_bytes, 113) ==
           LMB_SEG_STATUS_OUT_OF_ORDER);
    assert(lmb_seg_table_run_begin(table, &next, payload, payload_bytes, 113) ==
           LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_run_abort(table, &next) == LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_run_begin(table, &next, payload, payload_bytes, 114) ==
           LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_run_commit(table, &next, 115) == LMB_SEG_STATUS_OK);

    LmbSegOwner new_owner = first.owner;
    fill_id(&new_owner.lease_id, 180);
    new_owner.fencing_epoch = 2;
    new_owner.route_generation = 2;
    assert(lmb_seg_table_fence(table, &first.session_id, &new_owner, 116) ==
           LMB_SEG_STATUS_OK);
    LmbSegRun old_run = make_run(&first, 181, 2, 4, 2, tokens);
    assert(lmb_seg_table_run_begin(table, &old_run, payload, payload_bytes, 117) ==
           LMB_SEG_STATUS_STALE_OWNER);
    LmbSegRun new_run = old_run;
    fill_id(&new_run.request_id, 182);
    new_run.owner = new_owner;
    assert(lmb_seg_table_run_begin(table, &new_run, payload, payload_bytes, 117) ==
           LMB_SEG_STATUS_NEEDS_RESTORE);

    LmbSegTransfer restore;
    memset(&restore, 0, sizeof restore);
    restore.session_id = first.session_id;
    fill_id(&restore.request_id, 183);
    restore.owner = new_owner;
    restore.sequence = 2;
    restore.position = 4;
    restore.snapshot_size = 8;
    restore.chunk_len = 8;
    restore.flags = LMB_SEG_XFER_BEGIN | LMB_SEG_XFER_END;
    assert(lmb_seg_table_restore_commit(table, &restore, 118) ==
           LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_run_begin(table, &new_run, payload, payload_bytes, 119) ==
           LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_run_commit(table, &new_run, 120) == LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_restore_commit(table, &restore, 120) ==
           LMB_SEG_STATUS_OUT_OF_ORDER);
    assert(lmb_seg_table_fence(table, &first.session_id, &first.owner, 121) ==
           LMB_SEG_STATUS_STALE_OWNER);
    assert(lmb_seg_table_fence(table, &first.session_id, &new_owner, 121) ==
           LMB_SEG_STATUS_DUPLICATE);

    LmbSegTransfer snapshot = restore;
    fill_id(&snapshot.request_id, 184);
    snapshot.sequence = 3;
    snapshot.position = 6;
    snapshot.snapshot_size = 20;
    snapshot.chunk_len = 10;
    snapshot.flags = LMB_SEG_XFER_BEGIN;
    assert(lmb_seg_table_snapshot_check(table, &snapshot, 122) ==
           LMB_SEG_STATUS_OK);

    LmbSegControl close = {
        .session_id = first.session_id,
        .owner = new_owner,
        .sequence = 2,
    };
    fill_id(&close.request_id, 185);
    assert(lmb_seg_table_close(table, &close) == LMB_SEG_STATUS_OUT_OF_ORDER);
    close.sequence = 3;
    assert(lmb_seg_table_close(table, &close) == LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_state(table, &first.session_id, NULL, NULL, NULL) ==
           LMB_SEG_STATUS_NOT_FOUND);

    LmbSegId expired;
    assert(!lmb_seg_table_reap_expired(table, 1099, &expired));
    assert(lmb_seg_table_reap_expired(table, 1100, &expired));
    assert(lmb_seg_id_equal(&expired, &second.session_id));
    assert(lmb_seg_table_open(table, &third, 1101) == LMB_SEG_STATUS_OK);

    free(payload);
    lmb_seg_table_destroy(table);
}

typedef struct {
    LmbSegTable *table;
    LmbSegOpen open;
    unsigned seed;
    LmbSegStatus status;
} ConcurrentRun;

static void *concurrent_runs(void *opaque) {
    ConcurrentRun *work = (ConcurrentRun *)opaque;
    int32_t token = (int32_t)work->seed;
    size_t payload_bytes = (size_t)work->open.state_width *
                           lmb_seg_dtype_size(work->open.state_dtype);
    uint8_t *payload = (uint8_t *)malloc(payload_bytes);
    assert(payload);
    memset(payload, (int)(work->seed & 0xff), payload_bytes);
    work->status = LMB_SEG_STATUS_OK;
    for (uint64_t i = 0; i < 16; i++) {
        LmbSegRun run = make_run(&work->open, work->seed + (unsigned)i + 1,
                                 i, i, 1, &token);
        work->status = lmb_seg_table_run_begin(
            work->table, &run, payload, payload_bytes, 200 + i);
        if (work->status != LMB_SEG_STATUS_OK) break;
        work->status = lmb_seg_table_run_commit(work->table, &run, 200 + i);
        if (work->status != LMB_SEG_STATUS_OK) break;
    }
    free(payload);
    return NULL;
}

static void test_concurrent_sessions(void) {
    LmbSegTable *table = lmb_seg_table_create(2);
    assert(table);
    ConcurrentRun work[2] = {
        { .table = table, .open = make_open(&tiny_schemas[2], 190), .seed = 210 },
        { .table = table, .open = make_open(&tiny_schemas[4], 200), .seed = 240 },
    };
    assert(lmb_seg_table_open(table, &work[0].open, 100) == LMB_SEG_STATUS_OK);
    assert(lmb_seg_table_open(table, &work[1].open, 100) == LMB_SEG_STATUS_OK);
    pthread_t threads[2];
    assert(!pthread_create(&threads[0], NULL, concurrent_runs, &work[0]));
    assert(!pthread_create(&threads[1], NULL, concurrent_runs, &work[1]));
    assert(!pthread_join(threads[0], NULL));
    assert(!pthread_join(threads[1], NULL));
    assert(work[0].status == LMB_SEG_STATUS_OK);
    assert(work[1].status == LMB_SEG_STATUS_OK);
    for (size_t i = 0; i < 2; i++) {
        uint64_t sequence = 0, position = 0;
        assert(lmb_seg_table_state(table, &work[i].open.session_id,
                                   &sequence, &position, NULL) ==
               LMB_SEG_STATUS_OK);
        assert(sequence == 16 && position == 16);
    }
    lmb_seg_table_destroy(table);
}

int main(void) {
    test_open_matrix();
    test_run_wire();
    test_control_wire();
    test_session_table();
    test_concurrent_sessions();
    puts("SEGMENT V2: PASS (six state schemas, wire bounds, isolation, dedupe, fencing, TTL)");
    return 0;
}
