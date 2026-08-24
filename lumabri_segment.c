#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "lumabri_segment.h"

#include "lumabri_proto.h"
#include "lumabri_sha.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} SegBuf;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t off;
} SegCur;

static int seg_buf_reserve(SegBuf *buf, size_t extra) {
    if (extra > SIZE_MAX - buf->len) return -1;
    size_t need = buf->len + extra;
    if (need <= buf->cap) return 0;
    size_t cap = buf->cap ? buf->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    uint8_t *next = (uint8_t *)realloc(buf->data, cap);
    if (!next) return -1;
    buf->data = next;
    buf->cap = cap;
    return 0;
}

static int seg_buf_bytes(SegBuf *buf, const void *data, size_t len) {
    if ((len && !data) || seg_buf_reserve(buf, len)) return -1;
    if (len) memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

static int seg_buf_u16(SegBuf *buf, uint16_t value) {
    uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    return seg_buf_bytes(buf, bytes, sizeof bytes);
}

static int seg_buf_u32(SegBuf *buf, uint32_t value) {
    uint8_t bytes[4];
    lmb_put32(bytes, value);
    return seg_buf_bytes(buf, bytes, sizeof bytes);
}

static int seg_buf_u64(SegBuf *buf, uint64_t value) {
    return seg_buf_u32(buf, (uint32_t)value) ||
           seg_buf_u32(buf, (uint32_t)(value >> 32));
}

static int seg_buf_str(SegBuf *buf, const char *value, size_t capacity) {
    const char *end = value ? (const char *)memchr(value, '\0', capacity) : NULL;
    if (!end || end == value) return -1;
    size_t len = (size_t)(end - value);
    if (len > UINT16_MAX) return -1;
    return seg_buf_u16(buf, (uint16_t)len) || seg_buf_bytes(buf, value, len);
}

static int seg_cur_bytes(SegCur *cur, void *out, size_t len) {
    if (cur->off > cur->len || len > cur->len - cur->off) return -1;
    if (len) memcpy(out, cur->data + cur->off, len);
    cur->off += len;
    return 0;
}

static int seg_cur_u16(SegCur *cur, uint16_t *value) {
    uint8_t bytes[2];
    if (seg_cur_bytes(cur, bytes, sizeof bytes)) return -1;
    *value = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
    return 0;
}

static int seg_cur_u32(SegCur *cur, uint32_t *value) {
    uint8_t bytes[4];
    if (seg_cur_bytes(cur, bytes, sizeof bytes)) return -1;
    *value = lmb_get32(bytes);
    return 0;
}

static int seg_cur_u64(SegCur *cur, uint64_t *value) {
    uint32_t lo, hi;
    if (seg_cur_u32(cur, &lo) || seg_cur_u32(cur, &hi)) return -1;
    *value = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}

static int seg_cur_str(SegCur *cur, char *out, size_t capacity) {
    uint16_t len;
    if (!capacity || seg_cur_u16(cur, &len) || !len ||
        cur->off > cur->len || len > cur->len - cur->off ||
        (size_t)len >= capacity ||
        memchr(cur->data + cur->off, '\0', len) != NULL)
        return -1;
    memcpy(out, cur->data + cur->off, len);
    out[len] = '\0';
    cur->off += len;
    return 0;
}

static int seg_buf_prefix(SegBuf *buf) {
    return seg_buf_u32(buf, LMB_SEG_V2_MAGIC) ||
           seg_buf_u32(buf, LMB_SEG_V2_VERSION);
}

static int seg_cur_prefix(SegCur *cur) {
    uint32_t magic, version;
    return seg_cur_u32(cur, &magic) || seg_cur_u32(cur, &version) ||
           magic != LMB_SEG_V2_MAGIC || version != LMB_SEG_V2_VERSION;
}

static int seg_buf_id(SegBuf *buf, const LmbSegId *id) {
    return seg_buf_bytes(buf, id->bytes, sizeof id->bytes);
}

static int seg_cur_id(SegCur *cur, LmbSegId *id) {
    return seg_cur_bytes(cur, id->bytes, sizeof id->bytes);
}

static int seg_buf_owner(SegBuf *buf, const LmbSegOwner *owner) {
    return seg_buf_id(buf, &owner->lease_id) ||
           seg_buf_u64(buf, owner->fencing_epoch) ||
           seg_buf_u64(buf, owner->route_generation);
}

static int seg_cur_owner(SegCur *cur, LmbSegOwner *owner) {
    return seg_cur_id(cur, &owner->lease_id) ||
           seg_cur_u64(cur, &owner->fencing_epoch) ||
           seg_cur_u64(cur, &owner->route_generation);
}

static int seg_finish(SegBuf *buf, uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || buf->len > UINT32_MAX ||
        buf->len > LMB_MAX_SMALL_BODY) {
        free(buf->data);
        return -1;
    }
    *body = buf->data;
    *body_len = (uint32_t)buf->len;
    return 0;
}

int lmb_seg_id_is_zero(const LmbSegId *id) {
    if (!id) return 1;
    unsigned any = 0;
    for (size_t i = 0; i < sizeof id->bytes; i++) any |= id->bytes[i];
    return any == 0;
}

int lmb_seg_id_equal(const LmbSegId *a, const LmbSegId *b) {
    return a && b && memcmp(a->bytes, b->bytes, sizeof a->bytes) == 0;
}

size_t lmb_seg_dtype_size(uint32_t dtype) {
    switch (dtype) {
    case LMB_SEG_DTYPE_F32: return 4;
    case LMB_SEG_DTYPE_F16:
    case LMB_SEG_DTYPE_BF16: return 2;
    default: return 0;
    }
}

static int seg_root_is_zero(const uint8_t root[LMB_SEG_ROOT_BYTES]) {
    unsigned any = 0;
    for (size_t i = 0; i < LMB_SEG_ROOT_BYTES; i++) any |= root[i];
    return any == 0;
}

static int seg_string_valid(const char *value, size_t capacity) {
    const char *end = value ? (const char *)memchr(value, '\0', capacity) : NULL;
    return end && end != value;
}

static int seg_owner_valid(const LmbSegOwner *owner) {
    return owner && !lmb_seg_id_is_zero(&owner->lease_id) &&
           owner->fencing_epoch && owner->route_generation;
}

int lmb_seg_open_valid(const LmbSegOpen *open) {
    uint64_t backend = LMB_SEG_CAP_CPU | LMB_SEG_CAP_CUDA | LMB_SEG_CAP_HIP |
                       LMB_SEG_CAP_METAL | LMB_SEG_CAP_VULKAN;
    if (!open || lmb_seg_id_is_zero(&open->session_id) ||
        lmb_seg_id_is_zero(&open->request_id) || !seg_owner_valid(&open->owner) ||
        seg_root_is_zero(open->model_root) ||
        seg_root_is_zero(open->tokenizer_root) ||
        open->layer_begin >= open->layer_end ||
        !open->context_tokens || open->context_tokens > LMB_SEG_MAX_CONTEXT ||
        !open->max_rows || open->max_rows > LMB_SEG_MAX_ROWS ||
        !lmb_seg_dtype_size(open->state_dtype) ||
        !open->state_width || open->state_width > LMB_SEG_MAX_STATE_WIDTH ||
        open->ttl_ms < LMB_SEG_MIN_TTL_MS || open->ttl_ms > LMB_SEG_MAX_TTL_MS ||
        (open->capabilities & ~LMB_SEG_CAP_KNOWN_MASK) ||
        !(open->capabilities & backend) ||
        !(open->capabilities & LMB_SEG_CAP_MULTI_SESSION) ||
        !seg_string_valid(open->engine_id, sizeof open->engine_id) ||
        !seg_string_valid(open->state_schema, sizeof open->state_schema) ||
        !seg_string_valid(open->numeric_class, sizeof open->numeric_class))
        return 0;
    return 1;
}

int lmb_seg_run_payload_bytes(const LmbSegOpen *open,
                              const LmbSegRun *run, size_t *bytes) {
    if (!lmb_seg_open_valid(open) || !run || !bytes ||
        lmb_seg_id_is_zero(&run->session_id) ||
        lmb_seg_id_is_zero(&run->request_id) || !seg_owner_valid(&run->owner) ||
        !run->rows || run->rows > open->max_rows ||
        (run->token_count && run->token_count != run->rows) ||
        (!!run->token_count != !!run->token_ids) ||
        ((open->capabilities & LMB_SEG_CAP_TOKEN_IDS) &&
         run->token_count != run->rows) ||
        run->position > open->context_tokens ||
        run->rows > open->context_tokens - run->position)
        return -1;
    size_t element = lmb_seg_dtype_size(open->state_dtype);
    if (open->state_width > SIZE_MAX / run->rows ||
        (size_t)open->state_width * run->rows > SIZE_MAX / element)
        return -1;
    *bytes = (size_t)open->state_width * run->rows * element;
    return *bytes <= LMB_MAX_PAY ? 0 : -1;
}

int lmb_seg_open_encode(const LmbSegOpen *open,
                        uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || !lmb_seg_open_valid(open)) return -1;
    *body = NULL; *body_len = 0;
    SegBuf buf = {0};
    int bad = seg_buf_prefix(&buf) ||
              seg_buf_id(&buf, &open->session_id) ||
              seg_buf_id(&buf, &open->request_id) ||
              seg_buf_owner(&buf, &open->owner) ||
              seg_buf_bytes(&buf, open->model_root, sizeof open->model_root) ||
              seg_buf_bytes(&buf, open->tokenizer_root,
                            sizeof open->tokenizer_root) ||
              seg_buf_u32(&buf, open->layer_begin) ||
              seg_buf_u32(&buf, open->layer_end) ||
              seg_buf_u32(&buf, open->context_tokens) ||
              seg_buf_u32(&buf, open->max_rows) ||
              seg_buf_u32(&buf, open->state_dtype) ||
              seg_buf_u32(&buf, open->state_width) ||
              seg_buf_u32(&buf, open->ttl_ms) ||
              seg_buf_u64(&buf, open->capabilities) ||
              seg_buf_str(&buf, open->engine_id, sizeof open->engine_id) ||
              seg_buf_str(&buf, open->state_schema,
                          sizeof open->state_schema) ||
              seg_buf_str(&buf, open->numeric_class,
                          sizeof open->numeric_class);
    if (bad) { free(buf.data); return -1; }
    return seg_finish(&buf, body, body_len);
}

int lmb_seg_open_decode(const void *body, size_t body_len, LmbSegOpen *open) {
    if ((!body && body_len) || !open || body_len > LMB_MAX_SMALL_BODY) return -1;
    memset(open, 0, sizeof *open);
    SegCur cur = { (const uint8_t *)body, body_len, 0 };
    int bad = seg_cur_prefix(&cur) ||
              seg_cur_id(&cur, &open->session_id) ||
              seg_cur_id(&cur, &open->request_id) ||
              seg_cur_owner(&cur, &open->owner) ||
              seg_cur_bytes(&cur, open->model_root, sizeof open->model_root) ||
              seg_cur_bytes(&cur, open->tokenizer_root,
                            sizeof open->tokenizer_root) ||
              seg_cur_u32(&cur, &open->layer_begin) ||
              seg_cur_u32(&cur, &open->layer_end) ||
              seg_cur_u32(&cur, &open->context_tokens) ||
              seg_cur_u32(&cur, &open->max_rows) ||
              seg_cur_u32(&cur, &open->state_dtype) ||
              seg_cur_u32(&cur, &open->state_width) ||
              seg_cur_u32(&cur, &open->ttl_ms) ||
              seg_cur_u64(&cur, &open->capabilities) ||
              seg_cur_str(&cur, open->engine_id, sizeof open->engine_id) ||
              seg_cur_str(&cur, open->state_schema,
                          sizeof open->state_schema) ||
              seg_cur_str(&cur, open->numeric_class,
                          sizeof open->numeric_class) ||
              cur.off != cur.len;
    return bad || !lmb_seg_open_valid(open) ? -1 : 0;
}

static int seg_run_basic_valid(const LmbSegRun *run) {
    return run && !lmb_seg_id_is_zero(&run->session_id) &&
           !lmb_seg_id_is_zero(&run->request_id) && seg_owner_valid(&run->owner) &&
           run->sequence != UINT64_MAX &&
           run->rows && run->rows <= LMB_SEG_MAX_ROWS &&
           (!run->token_count || run->token_count == run->rows) &&
           (!!run->token_count == !!run->token_ids);
}

int lmb_seg_run_encode(const LmbSegRun *run,
                       uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || !seg_run_basic_valid(run)) return -1;
    *body = NULL; *body_len = 0;
    SegBuf buf = {0};
    int bad = seg_buf_prefix(&buf) || seg_buf_id(&buf, &run->session_id) ||
              seg_buf_id(&buf, &run->request_id) ||
              seg_buf_owner(&buf, &run->owner) ||
              seg_buf_u64(&buf, run->sequence) ||
              seg_buf_u64(&buf, run->position) ||
              seg_buf_u32(&buf, run->rows) ||
              seg_buf_u32(&buf, run->token_count);
    for (uint32_t i = 0; !bad && i < run->token_count; i++)
        bad = seg_buf_u32(&buf, (uint32_t)run->token_ids[i]);
    if (bad) { free(buf.data); return -1; }
    return seg_finish(&buf, body, body_len);
}

int lmb_seg_run_decode(const void *body, size_t body_len, LmbSegRun *run,
                       int32_t *token_ids, size_t token_capacity) {
    if ((!body && body_len) || !run || body_len > LMB_MAX_SMALL_BODY) return -1;
    memset(run, 0, sizeof *run);
    SegCur cur = { (const uint8_t *)body, body_len, 0 };
    int bad = seg_cur_prefix(&cur) || seg_cur_id(&cur, &run->session_id) ||
              seg_cur_id(&cur, &run->request_id) ||
              seg_cur_owner(&cur, &run->owner) ||
              seg_cur_u64(&cur, &run->sequence) ||
              seg_cur_u64(&cur, &run->position) ||
              seg_cur_u32(&cur, &run->rows) ||
              seg_cur_u32(&cur, &run->token_count) ||
              run->token_count > token_capacity ||
              (run->token_count && !token_ids);
    for (uint32_t i = 0; !bad && i < run->token_count; i++) {
        uint32_t value;
        if (seg_cur_u32(&cur, &value)) bad = 1;
        else token_ids[i] = (int32_t)value;
    }
    if (!bad) run->token_ids = run->token_count ? token_ids : NULL;
    return bad || cur.off != cur.len || !seg_run_basic_valid(run) ? -1 : 0;
}

static int seg_chunk_valid(uint64_t snapshot_size, uint64_t offset,
                           uint32_t chunk_len, uint32_t flags) {
    if ((flags & ~(LMB_SEG_XFER_BEGIN | LMB_SEG_XFER_END)) ||
        chunk_len > LMB_MAX_PAY || offset > snapshot_size ||
        chunk_len > snapshot_size - offset ||
        ((flags & LMB_SEG_XFER_BEGIN) && offset != 0) ||
        ((flags & LMB_SEG_XFER_END) &&
         chunk_len != snapshot_size - offset))
        return 0;
    return 1;
}

static int seg_transfer_valid(const LmbSegTransfer *transfer) {
    return transfer && !lmb_seg_id_is_zero(&transfer->session_id) &&
           !lmb_seg_id_is_zero(&transfer->request_id) &&
           seg_owner_valid(&transfer->owner) &&
           seg_chunk_valid(transfer->snapshot_size, transfer->offset,
                           transfer->chunk_len, transfer->flags);
}

int lmb_seg_transfer_encode(const LmbSegTransfer *transfer,
                            uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || !seg_transfer_valid(transfer)) return -1;
    *body = NULL; *body_len = 0;
    SegBuf buf = {0};
    int bad = seg_buf_prefix(&buf) ||
              seg_buf_id(&buf, &transfer->session_id) ||
              seg_buf_id(&buf, &transfer->request_id) ||
              seg_buf_owner(&buf, &transfer->owner) ||
              seg_buf_u64(&buf, transfer->sequence) ||
              seg_buf_u64(&buf, transfer->position) ||
              seg_buf_u64(&buf, transfer->snapshot_size) ||
              seg_buf_u64(&buf, transfer->offset) ||
              seg_buf_u32(&buf, transfer->chunk_len) ||
              seg_buf_u32(&buf, transfer->flags);
    if (bad) { free(buf.data); return -1; }
    return seg_finish(&buf, body, body_len);
}

int lmb_seg_transfer_decode(const void *body, size_t body_len,
                            LmbSegTransfer *transfer) {
    if ((!body && body_len) || !transfer || body_len > LMB_MAX_SMALL_BODY)
        return -1;
    memset(transfer, 0, sizeof *transfer);
    SegCur cur = { (const uint8_t *)body, body_len, 0 };
    int bad = seg_cur_prefix(&cur) ||
              seg_cur_id(&cur, &transfer->session_id) ||
              seg_cur_id(&cur, &transfer->request_id) ||
              seg_cur_owner(&cur, &transfer->owner) ||
              seg_cur_u64(&cur, &transfer->sequence) ||
              seg_cur_u64(&cur, &transfer->position) ||
              seg_cur_u64(&cur, &transfer->snapshot_size) ||
              seg_cur_u64(&cur, &transfer->offset) ||
              seg_cur_u32(&cur, &transfer->chunk_len) ||
              seg_cur_u32(&cur, &transfer->flags) || cur.off != cur.len;
    return bad || !seg_transfer_valid(transfer) ? -1 : 0;
}

static int seg_control_valid(const LmbSegControl *control) {
    return control && !lmb_seg_id_is_zero(&control->session_id) &&
           !lmb_seg_id_is_zero(&control->request_id) &&
           seg_owner_valid(&control->owner);
}

int lmb_seg_control_encode(const LmbSegControl *control,
                           uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || !seg_control_valid(control)) return -1;
    *body = NULL; *body_len = 0;
    SegBuf buf = {0};
    int bad = seg_buf_prefix(&buf) ||
              seg_buf_id(&buf, &control->session_id) ||
              seg_buf_id(&buf, &control->request_id) ||
              seg_buf_owner(&buf, &control->owner) ||
              seg_buf_u64(&buf, control->sequence);
    if (bad) { free(buf.data); return -1; }
    return seg_finish(&buf, body, body_len);
}

int lmb_seg_control_decode(const void *body, size_t body_len,
                           LmbSegControl *control) {
    if ((!body && body_len) || !control || body_len > LMB_MAX_SMALL_BODY)
        return -1;
    memset(control, 0, sizeof *control);
    SegCur cur = { (const uint8_t *)body, body_len, 0 };
    int bad = seg_cur_prefix(&cur) ||
              seg_cur_id(&cur, &control->session_id) ||
              seg_cur_id(&cur, &control->request_id) ||
              seg_cur_owner(&cur, &control->owner) ||
              seg_cur_u64(&cur, &control->sequence) || cur.off != cur.len;
    return bad || !seg_control_valid(control) ? -1 : 0;
}

static int seg_reply_valid(const LmbSegReply *reply) {
    return reply && !lmb_seg_id_is_zero(&reply->session_id) &&
           !lmb_seg_id_is_zero(&reply->request_id) &&
           reply->status <= LMB_SEG_STATUS_INTERNAL;
}

static int seg_buf_reply_fields(SegBuf *buf, const LmbSegReply *reply) {
    return seg_buf_id(buf, &reply->session_id) ||
           seg_buf_id(buf, &reply->request_id) ||
           seg_buf_u32(buf, reply->status) ||
           seg_buf_u32(buf, reply->flags) ||
           seg_buf_u64(buf, reply->next_sequence) ||
           seg_buf_u64(buf, reply->next_position) ||
           seg_buf_u64(buf, reply->fencing_epoch) ||
           seg_buf_u64(buf, reply->route_generation);
}

static int seg_cur_reply_fields(SegCur *cur, LmbSegReply *reply) {
    return seg_cur_id(cur, &reply->session_id) ||
           seg_cur_id(cur, &reply->request_id) ||
           seg_cur_u32(cur, &reply->status) ||
           seg_cur_u32(cur, &reply->flags) ||
           seg_cur_u64(cur, &reply->next_sequence) ||
           seg_cur_u64(cur, &reply->next_position) ||
           seg_cur_u64(cur, &reply->fencing_epoch) ||
           seg_cur_u64(cur, &reply->route_generation);
}

int lmb_seg_reply_encode(const LmbSegReply *reply,
                         uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || !seg_reply_valid(reply)) return -1;
    *body = NULL; *body_len = 0;
    SegBuf buf = {0};
    int bad = seg_buf_prefix(&buf) || seg_buf_reply_fields(&buf, reply);
    if (bad) { free(buf.data); return -1; }
    return seg_finish(&buf, body, body_len);
}

int lmb_seg_reply_decode(const void *body, size_t body_len,
                         LmbSegReply *reply) {
    if ((!body && body_len) || !reply || body_len > LMB_MAX_SMALL_BODY)
        return -1;
    memset(reply, 0, sizeof *reply);
    SegCur cur = { (const uint8_t *)body, body_len, 0 };
    int bad = seg_cur_prefix(&cur) || seg_cur_reply_fields(&cur, reply) ||
              cur.off != cur.len;
    return bad || !seg_reply_valid(reply) ? -1 : 0;
}

static int seg_transfer_reply_valid(const LmbSegTransferReply *reply) {
    if (!reply || !seg_reply_valid(&reply->reply)) return 0;
    if (reply->reply.status != LMB_SEG_STATUS_OK)
        return !reply->snapshot_size && !reply->offset && !reply->chunk_len &&
               !reply->transfer_flags;
    return seg_chunk_valid(reply->snapshot_size, reply->offset,
                           reply->chunk_len, reply->transfer_flags);
}

int lmb_seg_transfer_reply_encode(const LmbSegTransferReply *reply,
                                  uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || !seg_transfer_reply_valid(reply)) return -1;
    *body = NULL; *body_len = 0;
    SegBuf buf = {0};
    int bad = seg_buf_prefix(&buf) || seg_buf_reply_fields(&buf, &reply->reply) ||
              seg_buf_u64(&buf, reply->snapshot_size) ||
              seg_buf_u64(&buf, reply->offset) ||
              seg_buf_u32(&buf, reply->chunk_len) ||
              seg_buf_u32(&buf, reply->transfer_flags);
    if (bad) { free(buf.data); return -1; }
    return seg_finish(&buf, body, body_len);
}

int lmb_seg_transfer_reply_decode(const void *body, size_t body_len,
                                  LmbSegTransferReply *reply) {
    if ((!body && body_len) || !reply || body_len > LMB_MAX_SMALL_BODY)
        return -1;
    memset(reply, 0, sizeof *reply);
    SegCur cur = { (const uint8_t *)body, body_len, 0 };
    int bad = seg_cur_prefix(&cur) || seg_cur_reply_fields(&cur, &reply->reply) ||
              seg_cur_u64(&cur, &reply->snapshot_size) ||
              seg_cur_u64(&cur, &reply->offset) ||
              seg_cur_u32(&cur, &reply->chunk_len) ||
              seg_cur_u32(&cur, &reply->transfer_flags) || cur.off != cur.len;
    return bad || !seg_transfer_reply_valid(reply) ? -1 : 0;
}

typedef struct {
    int used;
    LmbSegId request_id;
    uint64_t sequence;
    uint64_t position;
    uint32_t rows;
    uint8_t digest[32];
} SegHistory;

typedef struct {
    uint8_t state;
    int busy;
    int needs_restore;
    LmbSegOpen open;
    uint64_t next_sequence;
    uint64_t next_position;
    uint64_t expires_at_ms;
    LmbSegRun pending;
    uint8_t pending_digest[32];
    SegHistory history[LMB_SEG_HISTORY_SLOTS];
    size_t history_next;
} SegSlot;

struct LmbSegTable {
    pthread_mutex_t lock;
    SegSlot *slots;
    size_t bucket_count;
    size_t max_sessions;
    size_t active_count;
};

enum { SEG_SLOT_EMPTY = 0, SEG_SLOT_ACTIVE = 1, SEG_SLOT_TOMBSTONE = 2 };

static uint64_t seg_deadline(uint64_t now_ms, uint32_t ttl_ms) {
    return now_ms > UINT64_MAX - ttl_ms ? UINT64_MAX : now_ms + ttl_ms;
}

static int seg_expired(const SegSlot *slot, uint64_t now_ms) {
    return now_ms >= slot->expires_at_ms;
}

static void seg_touch(SegSlot *slot, uint64_t now_ms) {
    slot->expires_at_ms = seg_deadline(now_ms, slot->open.ttl_ms);
}

static size_t seg_hash_id(const LmbSegId *session_id) {
    uint8_t digest[32];
    lmb_sha256(session_id->bytes, sizeof session_id->bytes, digest);
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)digest[i] << (8 * i);
    return (size_t)value;
}

/* Open addressing stays below 50% load (bucket_count = 2*quota+1). SHA-256
 * avoids an attacker choosing many session IDs with the same cheap hash. */
static SegSlot *seg_lookup(LmbSegTable *table, const LmbSegId *session_id,
                           int for_insert) {
    size_t start = seg_hash_id(session_id) % table->bucket_count;
    SegSlot *tombstone = NULL;
    for (size_t probe = 0; probe < table->bucket_count; probe++) {
        SegSlot *slot = &table->slots[(start + probe) % table->bucket_count];
        if (slot->state == SEG_SLOT_ACTIVE) {
            if (lmb_seg_id_equal(&slot->open.session_id, session_id)) return slot;
            continue;
        }
        if (slot->state == SEG_SLOT_TOMBSTONE) {
            if (!tombstone) tombstone = slot;
            continue;
        }
        return for_insert && tombstone ? tombstone : (for_insert ? slot : NULL);
    }
    return for_insert ? tombstone : NULL;
}

static SegSlot *seg_find(LmbSegTable *table, const LmbSegId *session_id) {
    SegSlot *slot = seg_lookup(table, session_id, 0);
    if (slot && slot->state == SEG_SLOT_ACTIVE) return slot;
    return NULL;
}

static int seg_owner_relation(const LmbSegOwner *candidate,
                              const LmbSegOwner *current) {
    int epoch = candidate->fencing_epoch < current->fencing_epoch ? -1 :
                candidate->fencing_epoch > current->fencing_epoch ? 1 : 0;
    int route = candidate->route_generation < current->route_generation ? -1 :
                candidate->route_generation > current->route_generation ? 1 : 0;
    if ((epoch < 0 && route > 0) || (epoch > 0 && route < 0)) return 2;
    if (epoch < 0 || route < 0) return -1;
    if (epoch > 0 || route > 0) return 1;
    return lmb_seg_id_equal(&candidate->lease_id, &current->lease_id) ? 0 : 2;
}

static LmbSegStatus seg_owner_status(const LmbSegOwner *candidate,
                                     const LmbSegOwner *current) {
    int relation = seg_owner_relation(candidate, current);
    if (relation < 0) return LMB_SEG_STATUS_STALE_OWNER;
    return relation == 0 ? LMB_SEG_STATUS_OK : LMB_SEG_STATUS_CONFLICT;
}

static int seg_open_equal(const LmbSegOpen *a, const LmbSegOpen *b) {
    return lmb_seg_id_equal(&a->session_id, &b->session_id) &&
           seg_owner_relation(&a->owner, &b->owner) == 0 &&
           memcmp(a->model_root, b->model_root, sizeof a->model_root) == 0 &&
           memcmp(a->tokenizer_root, b->tokenizer_root,
                  sizeof a->tokenizer_root) == 0 &&
           a->layer_begin == b->layer_begin && a->layer_end == b->layer_end &&
           a->context_tokens == b->context_tokens && a->max_rows == b->max_rows &&
           a->state_dtype == b->state_dtype && a->state_width == b->state_width &&
           a->ttl_ms == b->ttl_ms && a->capabilities == b->capabilities &&
           strcmp(a->engine_id, b->engine_id) == 0 &&
           strcmp(a->state_schema, b->state_schema) == 0 &&
           strcmp(a->numeric_class, b->numeric_class) == 0;
}

static void seg_run_digest(const LmbSegRun *run, const void *payload,
                           size_t payload_bytes, uint8_t digest[32]) {
    LmbSha sha;
    lmb_sha_init(&sha);
    lmb_sha_update(&sha, run->session_id.bytes, sizeof run->session_id.bytes);
    lmb_sha_update(&sha, run->owner.lease_id.bytes,
                   sizeof run->owner.lease_id.bytes);
    lmb_sha_le64(&sha, run->owner.fencing_epoch);
    lmb_sha_le64(&sha, run->owner.route_generation);
    lmb_sha_le64(&sha, run->sequence);
    lmb_sha_le64(&sha, run->position);
    lmb_sha_le32(&sha, run->rows);
    lmb_sha_le32(&sha, run->token_count);
    for (uint32_t i = 0; i < run->token_count; i++)
        lmb_sha_le32(&sha, (uint32_t)run->token_ids[i]);
    lmb_sha_update(&sha, payload, payload_bytes);
    lmb_sha_final(&sha, digest);
}

static int seg_run_same(const LmbSegRun *a, const LmbSegRun *b) {
    return lmb_seg_id_equal(&a->session_id, &b->session_id) &&
           lmb_seg_id_equal(&a->request_id, &b->request_id) &&
           seg_owner_relation(&a->owner, &b->owner) == 0 &&
           a->sequence == b->sequence && a->position == b->position &&
           a->rows == b->rows && a->token_count == b->token_count;
}

LmbSegTable *lmb_seg_table_create(size_t capacity) {
    if (!capacity || capacity > (SIZE_MAX - 1) / 2) return NULL;
    size_t bucket_count = capacity * 2 + 1;
    if (bucket_count > SIZE_MAX / sizeof(SegSlot)) return NULL;
    LmbSegTable *table = (LmbSegTable *)calloc(1, sizeof *table);
    if (!table) return NULL;
    table->slots = (SegSlot *)calloc(bucket_count, sizeof *table->slots);
    if (!table->slots || pthread_mutex_init(&table->lock, NULL)) {
        free(table->slots);
        free(table);
        return NULL;
    }
    table->bucket_count = bucket_count;
    table->max_sessions = capacity;
    return table;
}

void lmb_seg_table_destroy(LmbSegTable *table) {
    if (!table) return;
    pthread_mutex_destroy(&table->lock);
    free(table->slots);
    free(table);
}

LmbSegStatus lmb_seg_table_open(LmbSegTable *table,
                                const LmbSegOpen *open, uint64_t now_ms) {
    if (!table || !lmb_seg_open_valid(open)) return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, &open->session_id);
    if (slot) {
        LmbSegStatus status;
        if (seg_expired(slot, now_ms)) status = LMB_SEG_STATUS_EXPIRED;
        else if (lmb_seg_id_equal(&slot->open.request_id, &open->request_id) &&
                 seg_open_equal(&slot->open, open)) {
            seg_touch(slot, now_ms);
            status = LMB_SEG_STATUS_DUPLICATE;
        } else {
            LmbSegStatus owner = seg_owner_status(&open->owner, &slot->open.owner);
            status = owner == LMB_SEG_STATUS_OK ? LMB_SEG_STATUS_CONFLICT : owner;
        }
        pthread_mutex_unlock(&table->lock);
        return status;
    }
    if (table->active_count >= table->max_sessions ||
        !(slot = seg_lookup(table, &open->session_id, 1))) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_QUOTA;
    }
    memset(slot, 0, sizeof *slot);
    slot->state = SEG_SLOT_ACTIVE;
    slot->open = *open;
    table->active_count++;
    seg_touch(slot, now_ms);
    pthread_mutex_unlock(&table->lock);
    return LMB_SEG_STATUS_OK;
}

LmbSegStatus lmb_seg_table_run_begin(LmbSegTable *table,
                                     const LmbSegRun *run,
                                     const void *payload,
                                     size_t payload_bytes, uint64_t now_ms) {
    if (!table || !run || (!payload && payload_bytes))
        return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, &run->session_id);
    if (!slot) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_NOT_FOUND;
    }
    if (seg_expired(slot, now_ms)) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_EXPIRED;
    }
    LmbSegStatus owner = seg_owner_status(&run->owner, &slot->open.owner);
    if (owner != LMB_SEG_STATUS_OK) {
        pthread_mutex_unlock(&table->lock);
        return owner;
    }
    size_t expected = 0;
    if (lmb_seg_run_payload_bytes(&slot->open, run, &expected) ||
        expected != payload_bytes) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_BAD_REQUEST;
    }
    uint8_t digest[32];
    seg_run_digest(run, payload, payload_bytes, digest);
    for (size_t i = 0; i < LMB_SEG_HISTORY_SLOTS; i++) {
        SegHistory *history = &slot->history[i];
        if (!history->used ||
            !lmb_seg_id_equal(&history->request_id, &run->request_id))
            continue;
        LmbSegStatus status =
            history->sequence == run->sequence &&
            history->position == run->position && history->rows == run->rows &&
            memcmp(history->digest, digest, sizeof digest) == 0
                ? LMB_SEG_STATUS_DUPLICATE : LMB_SEG_STATUS_CONFLICT;
        if (status == LMB_SEG_STATUS_DUPLICATE) seg_touch(slot, now_ms);
        pthread_mutex_unlock(&table->lock);
        return status;
    }
    if (slot->busy) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_BUSY;
    }
    if (slot->needs_restore) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_NEEDS_RESTORE;
    }
    if (run->sequence != slot->next_sequence ||
        run->position != slot->next_position) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_OUT_OF_ORDER;
    }
    slot->busy = 1;
    slot->pending = *run;
    slot->pending.token_ids = NULL;
    memcpy(slot->pending_digest, digest, sizeof digest);
    pthread_mutex_unlock(&table->lock);
    return LMB_SEG_STATUS_OK;
}

LmbSegStatus lmb_seg_table_run_commit(LmbSegTable *table,
                                      const LmbSegRun *run, uint64_t now_ms) {
    if (!table || !run) return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, &run->session_id);
    if (!slot) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_NOT_FOUND;
    }
    if (!slot->busy || !seg_run_same(&slot->pending, run)) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_CONFLICT;
    }
    SegHistory *history = &slot->history[slot->history_next];
    memset(history, 0, sizeof *history);
    history->used = 1;
    history->request_id = run->request_id;
    history->sequence = run->sequence;
    history->position = run->position;
    history->rows = run->rows;
    memcpy(history->digest, slot->pending_digest, sizeof history->digest);
    slot->history_next = (slot->history_next + 1) % LMB_SEG_HISTORY_SLOTS;
    slot->next_sequence++;
    slot->next_position += run->rows;
    slot->busy = 0;
    memset(&slot->pending, 0, sizeof slot->pending);
    memset(slot->pending_digest, 0, sizeof slot->pending_digest);
    seg_touch(slot, now_ms);
    pthread_mutex_unlock(&table->lock);
    return LMB_SEG_STATUS_OK;
}

LmbSegStatus lmb_seg_table_run_abort(LmbSegTable *table,
                                     const LmbSegRun *run) {
    if (!table || !run) return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, &run->session_id);
    if (!slot) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_NOT_FOUND;
    }
    if (!slot->busy || !seg_run_same(&slot->pending, run)) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_CONFLICT;
    }
    slot->busy = 0;
    memset(&slot->pending, 0, sizeof slot->pending);
    memset(slot->pending_digest, 0, sizeof slot->pending_digest);
    pthread_mutex_unlock(&table->lock);
    return LMB_SEG_STATUS_OK;
}

LmbSegStatus lmb_seg_table_snapshot_check(LmbSegTable *table,
                                          const LmbSegTransfer *transfer,
                                          uint64_t now_ms) {
    if (!table || !seg_transfer_valid(transfer))
        return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, &transfer->session_id);
    LmbSegStatus status = LMB_SEG_STATUS_OK;
    if (!slot) status = LMB_SEG_STATUS_NOT_FOUND;
    else if (seg_expired(slot, now_ms)) status = LMB_SEG_STATUS_EXPIRED;
    else if (!(slot->open.capabilities & LMB_SEG_CAP_SNAPSHOT))
        status = LMB_SEG_STATUS_UNSUPPORTED;
    else if ((status = seg_owner_status(&transfer->owner, &slot->open.owner)) !=
             LMB_SEG_STATUS_OK) { /* status already set */ }
    else if (slot->busy) status = LMB_SEG_STATUS_BUSY;
    else if (slot->needs_restore) status = LMB_SEG_STATUS_NEEDS_RESTORE;
    else if (transfer->sequence != slot->next_sequence ||
             transfer->position != slot->next_position)
        status = LMB_SEG_STATUS_OUT_OF_ORDER;
    else seg_touch(slot, now_ms);
    pthread_mutex_unlock(&table->lock);
    return status;
}

LmbSegStatus lmb_seg_table_restore_commit(LmbSegTable *table,
                                          const LmbSegTransfer *transfer,
                                          uint64_t now_ms) {
    if (!table || !seg_transfer_valid(transfer) ||
        transfer->flags != (LMB_SEG_XFER_BEGIN | LMB_SEG_XFER_END) ||
        transfer->offset || transfer->snapshot_size != transfer->chunk_len)
        return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, &transfer->session_id);
    LmbSegStatus status = LMB_SEG_STATUS_OK;
    if (!slot) status = LMB_SEG_STATUS_NOT_FOUND;
    else if (seg_expired(slot, now_ms)) status = LMB_SEG_STATUS_EXPIRED;
    else if (!(slot->open.capabilities & LMB_SEG_CAP_SNAPSHOT))
        status = LMB_SEG_STATUS_UNSUPPORTED;
    else if (slot->busy) status = LMB_SEG_STATUS_BUSY;
    else if (transfer->position > slot->open.context_tokens ||
             transfer->sequence == UINT64_MAX)
        status = LMB_SEG_STATUS_BAD_REQUEST;
    else {
        int relation = seg_owner_relation(&transfer->owner, &slot->open.owner);
        if (relation < 0) status = LMB_SEG_STATUS_STALE_OWNER;
        else if (relation == 2) status = LMB_SEG_STATUS_CONFLICT;
        else if (relation == 0 &&
                 (transfer->sequence < slot->next_sequence ||
                  transfer->position < slot->next_position))
            status = LMB_SEG_STATUS_OUT_OF_ORDER;
        else {
            slot->open.owner = transfer->owner;
            slot->next_sequence = transfer->sequence;
            slot->next_position = transfer->position;
            slot->needs_restore = 0;
            memset(slot->history, 0, sizeof slot->history);
            slot->history_next = 0;
            seg_touch(slot, now_ms);
        }
    }
    pthread_mutex_unlock(&table->lock);
    return status;
}

LmbSegStatus lmb_seg_table_fence(LmbSegTable *table,
                                 const LmbSegId *session_id,
                                 const LmbSegOwner *new_owner,
                                 uint64_t now_ms) {
    if (!table || lmb_seg_id_is_zero(session_id) || !seg_owner_valid(new_owner))
        return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, session_id);
    LmbSegStatus status;
    if (!slot) status = LMB_SEG_STATUS_NOT_FOUND;
    else if (seg_expired(slot, now_ms)) status = LMB_SEG_STATUS_EXPIRED;
    else if (slot->busy) status = LMB_SEG_STATUS_BUSY;
    else {
        int relation = seg_owner_relation(new_owner, &slot->open.owner);
        if (relation < 0) status = LMB_SEG_STATUS_STALE_OWNER;
        else if (relation == 0) status = LMB_SEG_STATUS_DUPLICATE;
        else if (relation == 2) status = LMB_SEG_STATUS_CONFLICT;
        else {
            slot->open.owner = *new_owner;
            slot->needs_restore = 1;
            memset(slot->history, 0, sizeof slot->history);
            slot->history_next = 0;
            seg_touch(slot, now_ms);
            status = LMB_SEG_STATUS_OK;
        }
    }
    pthread_mutex_unlock(&table->lock);
    return status;
}

LmbSegStatus lmb_seg_table_close(LmbSegTable *table,
                                 const LmbSegControl *control) {
    if (!table || !seg_control_valid(control)) return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, &control->session_id);
    LmbSegStatus status = LMB_SEG_STATUS_OK;
    if (!slot) status = LMB_SEG_STATUS_NOT_FOUND;
    else if ((status = seg_owner_status(&control->owner, &slot->open.owner)) !=
             LMB_SEG_STATUS_OK) { /* status already set */ }
    else if (slot->busy) status = LMB_SEG_STATUS_BUSY;
    else if (control->sequence != slot->next_sequence)
        status = LMB_SEG_STATUS_OUT_OF_ORDER;
    else {
        memset(slot, 0, sizeof *slot);
        slot->state = SEG_SLOT_TOMBSTONE;
        table->active_count--;
    }
    pthread_mutex_unlock(&table->lock);
    return status;
}

LmbSegStatus lmb_seg_table_state(LmbSegTable *table,
                                 const LmbSegId *session_id,
                                 uint64_t *next_sequence,
                                 uint64_t *next_position,
                                 int *needs_restore) {
    if (!table || lmb_seg_id_is_zero(session_id))
        return LMB_SEG_STATUS_BAD_REQUEST;
    pthread_mutex_lock(&table->lock);
    SegSlot *slot = seg_find(table, session_id);
    if (!slot) {
        pthread_mutex_unlock(&table->lock);
        return LMB_SEG_STATUS_NOT_FOUND;
    }
    if (next_sequence) *next_sequence = slot->next_sequence;
    if (next_position) *next_position = slot->next_position;
    if (needs_restore) *needs_restore = slot->needs_restore;
    pthread_mutex_unlock(&table->lock);
    return LMB_SEG_STATUS_OK;
}

int lmb_seg_table_reap_expired(LmbSegTable *table, uint64_t now_ms,
                               LmbSegId *session_id) {
    if (!table || !session_id) return 0;
    pthread_mutex_lock(&table->lock);
    for (size_t i = 0; i < table->bucket_count; i++) {
        SegSlot *slot = &table->slots[i];
        if (slot->state == SEG_SLOT_ACTIVE && !slot->busy &&
            seg_expired(slot, now_ms)) {
            *session_id = slot->open.session_id;
            memset(slot, 0, sizeof *slot);
            slot->state = SEG_SLOT_TOMBSTONE;
            table->active_count--;
            pthread_mutex_unlock(&table->lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&table->lock);
    return 0;
}
