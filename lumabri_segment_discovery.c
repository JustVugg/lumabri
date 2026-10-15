#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "lumabri_segment_discovery.h"

#include "lumabri_proto.h"
#include "lumabri_secure.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const uint8_t *p; size_t len, off; } DiscCur;

static uint64_t disc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int disc_str_valid(const char *s, size_t cap) {
    const char *end = s ? (const char *)memchr(s, '\0', cap) : NULL;
    return end && end != s;
}

static int disc_root_valid(const uint8_t root[LMB_SEG_ROOT_BYTES]) {
    unsigned any = 0;
    for (size_t i = 0; i < LMB_SEG_ROOT_BYTES; i++) any |= root[i];
    return any != 0;
}

static int disc_backend_valid(uint64_t caps) {
    const uint64_t backends = LMB_SEG_CAP_CPU | LMB_SEG_CAP_CUDA |
                              LMB_SEG_CAP_HIP | LMB_SEG_CAP_METAL |
                              LMB_SEG_CAP_VULKAN;
    return (caps & backends) != 0;
}

int lmb_seg_advert_valid(const LmbSegAdvert *a) {
    if (!a || !disc_str_valid(a->peer_name, sizeof a->peer_name) ||
        !disc_str_valid(a->addr, sizeof a->addr) ||
        !disc_str_valid(a->model, sizeof a->model) ||
        !disc_root_valid(a->model_root) ||
        !disc_root_valid(a->tokenizer_root) ||
        a->layer_begin >= a->layer_end ||
        !a->max_context || a->max_context > LMB_SEG_MAX_CONTEXT ||
        !a->max_rows || a->max_rows > LMB_SEG_MAX_ROWS ||
        !lmb_seg_dtype_size(a->state_dtype) ||
        !a->state_width || a->state_width > LMB_SEG_MAX_STATE_WIDTH ||
        !a->max_sessions || a->active_sessions > a->max_sessions ||
        (a->flags & ~(LMB_SEG_ADVERT_DRAINING |
                      LMB_SEG_ADVERT_FALLBACK |
                      LMB_SEG_ADVERT_RELAY_ONLY)) ||
        (a->capabilities & ~LMB_SEG_CAP_KNOWN_MASK) ||
        !(a->capabilities & LMB_SEG_CAP_RANGE_NATIVE) ||
        !(a->capabilities & LMB_SEG_CAP_MULTI_SESSION) ||
        !disc_backend_valid(a->capabilities) ||
        !disc_str_valid(a->engine_id, sizeof a->engine_id) ||
        !disc_str_valid(a->state_schema, sizeof a->state_schema) ||
        !disc_str_valid(a->numeric_class, sizeof a->numeric_class))
        return 0;
    return 1;
}

int lmb_seg_query_valid(const LmbSegQuery *q) {
    if (!q || !disc_str_valid(q->model, sizeof q->model) ||
        !disc_root_valid(q->model_root) ||
        !disc_root_valid(q->tokenizer_root) ||
        q->layer_begin >= q->layer_end ||
        !q->context_tokens || q->context_tokens > LMB_SEG_MAX_CONTEXT ||
        !q->rows || q->rows > LMB_SEG_MAX_ROWS ||
        !lmb_seg_dtype_size(q->state_dtype) ||
        !q->state_width || q->state_width > LMB_SEG_MAX_STATE_WIDTH ||
        (q->required_capabilities & ~LMB_SEG_CAP_KNOWN_MASK) ||
        !disc_str_valid(q->engine_id, sizeof q->engine_id) ||
        !disc_str_valid(q->state_schema, sizeof q->state_schema) ||
        !disc_str_valid(q->numeric_class, sizeof q->numeric_class))
        return 0;
    return 1;
}

int lmb_seg_advert_compatible(const LmbSegAdvert *a, const LmbSegQuery *q) {
    return lmb_seg_advert_valid(a) && lmb_seg_query_valid(q) &&
           !(a->flags & LMB_SEG_ADVERT_DRAINING) &&
           !strcmp(a->model, q->model) &&
           !memcmp(a->model_root, q->model_root, LMB_SEG_ROOT_BYTES) &&
           !memcmp(a->tokenizer_root, q->tokenizer_root, LMB_SEG_ROOT_BYTES) &&
           a->layer_begin < q->layer_end && a->layer_end > q->layer_begin &&
           a->max_context >= q->context_tokens && a->max_rows >= q->rows &&
           a->state_dtype == q->state_dtype && a->state_width == q->state_width &&
           (a->capabilities & q->required_capabilities) ==
               q->required_capabilities &&
           !strcmp(a->engine_id, q->engine_id) &&
           !strcmp(a->state_schema, q->state_schema) &&
           !strcmp(a->numeric_class, q->numeric_class);
}

int lmb_seg_route_complete(const LmbSegRouteSnapshot *s,
                           const LmbSegQuery *q) {
    if (!s || !lmb_seg_query_valid(q) || s->count > LMB_SEG_ROUTE_MAX) return 0;
    /* Coverage is not interval union: executors cannot run half of an
     * advertised range, and overlapping ranges would execute a layer twice.
     * Track the exact boundaries reachable from query.begin instead. */
    uint32_t reachable[LMB_SEG_ROUTE_MAX + 1];
    size_t reachable_count = 1;
    reachable[0] = q->layer_begin;
    for (uint32_t pass = 0; pass < s->count; pass++) {
        int changed = 0;
        for (uint32_t i = 0; i < s->count; i++) {
            const LmbSegAdvert *a = &s->entries[i].advert;
            if (!lmb_seg_advert_compatible(a, q) ||
                a->layer_begin < q->layer_begin || a->layer_end > q->layer_end)
                continue;
            int begin_reachable = 0, end_known = 0;
            for (size_t j = 0; j < reachable_count; j++) {
                begin_reachable |= reachable[j] == a->layer_begin;
                end_known |= reachable[j] == a->layer_end;
            }
            if (begin_reachable && !end_known &&
                reachable_count < LMB_SEG_ROUTE_MAX + 1) {
                reachable[reachable_count++] = a->layer_end;
                changed = 1;
            }
        }
        if (!changed) break;
    }
    for (size_t i = 0; i < reachable_count; i++)
        if (reachable[i] == q->layer_end) return 1;
    return 0;
}

static int disc_cur_bytes(DiscCur *c, void *out, size_t n) {
    if (c->off > c->len || n > c->len - c->off) return -1;
    if (n) memcpy(out, c->p + c->off, n);
    c->off += n;
    return 0;
}

static int disc_cur_u16(DiscCur *c, uint16_t *v) {
    uint8_t p[2];
    if (disc_cur_bytes(c, p, sizeof p)) return -1;
    *v = (uint16_t)(p[0] | (uint16_t)p[1] << 8);
    return 0;
}

static int disc_cur_u32(DiscCur *c, uint32_t *v) {
    uint8_t p[4];
    if (disc_cur_bytes(c, p, sizeof p)) return -1;
    *v = lmb_get32(p);
    return 0;
}

static int disc_cur_u64(DiscCur *c, uint64_t *v) {
    uint32_t lo, hi;
    if (disc_cur_u32(c, &lo) || disc_cur_u32(c, &hi)) return -1;
    *v = (uint64_t)lo | (uint64_t)hi << 32;
    return 0;
}

static int disc_cur_str(DiscCur *c, char *out, size_t cap) {
    uint16_t n;
    if (!cap || disc_cur_u16(c, &n) || !n || n >= cap ||
        c->off > c->len || n > c->len - c->off ||
        memchr(c->p + c->off, '\0', n))
        return -1;
    memcpy(out, c->p + c->off, n); out[n] = 0; c->off += n;
    return 0;
}

static int disc_buf_prefix(LmbBuf *b) {
    return lmb_buf_u32(b, LMB_SEG_DISCOVERY_MAGIC) ||
           lmb_buf_u32(b, LMB_SEG_DISCOVERY_VERSION);
}

static int disc_cur_prefix(DiscCur *c) {
    uint32_t magic, version;
    return disc_cur_u32(c, &magic) || disc_cur_u32(c, &version) ||
           magic != LMB_SEG_DISCOVERY_MAGIC ||
           version != LMB_SEG_DISCOVERY_VERSION;
}

static int disc_buf_advert(LmbBuf *b, const LmbSegAdvert *a) {
    return lmb_buf_str(b, a->peer_name) || lmb_buf_str(b, a->addr) ||
           lmb_buf_str(b, a->model) ||
           lmb_buf_bytes(b, a->model_root, sizeof a->model_root) ||
           lmb_buf_bytes(b, a->tokenizer_root, sizeof a->tokenizer_root) ||
           lmb_buf_u32(b, a->layer_begin) || lmb_buf_u32(b, a->layer_end) ||
           lmb_buf_u32(b, a->max_context) || lmb_buf_u32(b, a->max_rows) ||
           lmb_buf_u32(b, a->state_dtype) || lmb_buf_u32(b, a->state_width) ||
           lmb_buf_u32(b, a->max_sessions) ||
           lmb_buf_u32(b, a->active_sessions) ||
           lmb_buf_u32(b, a->queue_depth) || lmb_buf_u32(b, a->inflight) ||
           lmb_buf_u32(b, a->flags) || lmb_buf_u64(b, a->capabilities) ||
           lmb_buf_u64(b, a->resident_ram_bytes) ||
           lmb_buf_u64(b, a->resident_vram_bytes) ||
           lmb_buf_str(b, a->engine_id) || lmb_buf_str(b, a->state_schema) ||
           lmb_buf_str(b, a->numeric_class);
}

static int disc_cur_advert(DiscCur *c, LmbSegAdvert *a) {
    return disc_cur_str(c, a->peer_name, sizeof a->peer_name) ||
           disc_cur_str(c, a->addr, sizeof a->addr) ||
           disc_cur_str(c, a->model, sizeof a->model) ||
           disc_cur_bytes(c, a->model_root, sizeof a->model_root) ||
           disc_cur_bytes(c, a->tokenizer_root, sizeof a->tokenizer_root) ||
           disc_cur_u32(c, &a->layer_begin) || disc_cur_u32(c, &a->layer_end) ||
           disc_cur_u32(c, &a->max_context) || disc_cur_u32(c, &a->max_rows) ||
           disc_cur_u32(c, &a->state_dtype) || disc_cur_u32(c, &a->state_width) ||
           disc_cur_u32(c, &a->max_sessions) ||
           disc_cur_u32(c, &a->active_sessions) ||
           disc_cur_u32(c, &a->queue_depth) || disc_cur_u32(c, &a->inflight) ||
           disc_cur_u32(c, &a->flags) || disc_cur_u64(c, &a->capabilities) ||
           disc_cur_u64(c, &a->resident_ram_bytes) ||
           disc_cur_u64(c, &a->resident_vram_bytes) ||
           disc_cur_str(c, a->engine_id, sizeof a->engine_id) ||
           disc_cur_str(c, a->state_schema, sizeof a->state_schema) ||
           disc_cur_str(c, a->numeric_class, sizeof a->numeric_class);
}

static int disc_finish(LmbBuf *b, uint8_t **body, uint32_t *body_len) {
    if (!body || !body_len || b->len > UINT32_MAX ||
        b->len > LMB_MAX_CONTROL_BODY) {
        free(b->p); return -1;
    }
    *body = b->p; *body_len = (uint32_t)b->len;
    return 0;
}

#define LMB_SEG_COMPUTE_MAGIC 0x31435347u /* "GSC1" */
#define LMB_SEG_COMPUTE_VERSION 1u

int lmb_seg_compute_valid(const LmbSegCompute *c) {
    return c && !lmb_seg_id_is_zero(&c->lease_id) && c->fencing_epoch &&
           c->samples && c->ewma_us && c->ewma_us <= LMB_SEG_COMPUTE_MAX_US;
}

void lmb_seg_compute_observe(LmbSegCompute *c, uint64_t sample_us) {
    if (!c || !sample_us) return;
    if (sample_us > LMB_SEG_COMPUTE_MAX_US)
        sample_us = LMB_SEG_COMPUTE_MAX_US;
    if (!c->samples) c->ewma_us = sample_us;
    else c->ewma_us = (c->ewma_us * 7u + sample_us) / 8u;
    if (c->samples < UINT32_MAX) c->samples++;
}

static int disc_compute_prefix(LmbBuf *b, uint32_t count) {
    return lmb_buf_u32(b, LMB_SEG_COMPUTE_MAGIC) ||
           lmb_buf_u32(b, LMB_SEG_COMPUTE_VERSION) ||
           lmb_buf_u32(b, count);
}

static int disc_compute_header(DiscCur *c, uint32_t *count) {
    uint32_t magic, version;
    return disc_cur_u32(c, &magic) || disc_cur_u32(c, &version) ||
           disc_cur_u32(c, count) || magic != LMB_SEG_COMPUTE_MAGIC ||
           version != LMB_SEG_COMPUTE_VERSION || *count > LMB_SEG_ROUTE_MAX;
}

static int disc_compute_key_write(LmbBuf *b, const LmbSegCompute *c) {
    return lmb_buf_bytes(b, c->lease_id.bytes, sizeof c->lease_id.bytes) ||
           lmb_buf_u64(b, c->fencing_epoch);
}

static int disc_compute_key_read(DiscCur *cursor, LmbSegCompute *c) {
    return disc_cur_bytes(cursor, c->lease_id.bytes, sizeof c->lease_id.bytes) ||
           disc_cur_u64(cursor, &c->fencing_epoch) ||
           lmb_seg_id_is_zero(&c->lease_id) || !c->fencing_epoch;
}

static int disc_compute_value_write(LmbBuf *b, const LmbSegCompute *c) {
    return disc_compute_key_write(b, c) || lmb_buf_u32(b, c->samples) ||
           lmb_buf_u64(b, c->ewma_us);
}

static int disc_compute_value_read(DiscCur *cursor, LmbSegCompute *c) {
    return disc_compute_key_read(cursor, c) ||
           disc_cur_u32(cursor, &c->samples) ||
           disc_cur_u64(cursor, &c->ewma_us) || !lmb_seg_compute_valid(c);
}

int lmb_seg_compute_put_encode(const LmbSegCompute *c,
                               uint8_t **body, uint32_t *body_len) {
    if (!lmb_seg_compute_valid(c)) return -1;
    LmbBuf b = {0};
    if (disc_compute_prefix(&b, 1u) || disc_compute_value_write(&b, c)) {
        free(b.p); return -1;
    }
    return disc_finish(&b, body, body_len);
}

int lmb_seg_compute_put_decode(const void *body, size_t body_len,
                               LmbSegCompute *c) {
    if ((!body && body_len) || !c) return -1;
    DiscCur cursor = {body, body_len, 0};
    uint32_t count = 0;
    memset(c, 0, sizeof *c);
    return disc_compute_header(&cursor, &count) || count != 1u ||
           disc_compute_value_read(&cursor, c) || cursor.off != cursor.len ? -1 : 0;
}

int lmb_seg_compute_get_encode(const LmbSegRouteSnapshot *snapshot,
                               uint8_t **body, uint32_t *body_len) {
    if (!snapshot || snapshot->count > LMB_SEG_ROUTE_MAX) return -1;
    LmbBuf b = {0};
    int bad = disc_compute_prefix(&b, snapshot->count);
    for (uint32_t i = 0; !bad && i < snapshot->count; i++) {
        LmbSegCompute key = {
            .lease_id = snapshot->entries[i].owner.lease_id,
            .fencing_epoch = snapshot->entries[i].owner.fencing_epoch,
        };
        bad = lmb_seg_id_is_zero(&key.lease_id) || !key.fencing_epoch ||
              disc_compute_key_write(&b, &key);
    }
    if (bad) { free(b.p); return -1; }
    return disc_finish(&b, body, body_len);
}

int lmb_seg_compute_get_request_decode(const void *body, size_t body_len,
                                       LmbSegCompute *requested, uint32_t *count) {
    if ((!body && body_len) || !requested || !count) return -1;
    DiscCur cursor = {body, body_len, 0};
    if (disc_compute_header(&cursor, count)) return -1;
    for (uint32_t i = 0; i < *count; i++) {
        memset(&requested[i], 0, sizeof requested[i]);
        if (disc_compute_key_read(&cursor, &requested[i])) return -1;
        for (uint32_t j = 0; j < i; j++)
            if (lmb_seg_id_equal(&requested[i].lease_id, &requested[j].lease_id) &&
                requested[i].fencing_epoch == requested[j].fencing_epoch) return -1;
    }
    return cursor.off == cursor.len ? 0 : -1;
}

int lmb_seg_compute_get_reply_encode(const LmbSegCompute *values, uint32_t count,
                                     uint8_t **body, uint32_t *body_len) {
    if (count > LMB_SEG_ROUTE_MAX || (count && !values)) return -1;
    LmbBuf b = {0};
    int bad = disc_compute_prefix(&b, count);
    for (uint32_t i = 0; !bad && i < count; i++)
        bad = !lmb_seg_compute_valid(&values[i]) ||
              disc_compute_value_write(&b, &values[i]);
    if (bad) { free(b.p); return -1; }
    return disc_finish(&b, body, body_len);
}

int lmb_seg_compute_get_decode(const void *body, size_t body_len,
                               LmbSegRouteSnapshot *snapshot) {
    if ((!body && body_len) || !snapshot || snapshot->count > LMB_SEG_ROUTE_MAX)
        return -1;
    DiscCur cursor = {body, body_len, 0};
    uint32_t count = 0;
    LmbSegCompute parsed[LMB_SEG_ROUTE_MAX];
    int indices[LMB_SEG_ROUTE_MAX];
    memset(parsed, 0, sizeof parsed);
    if (disc_compute_header(&cursor, &count) || count > snapshot->count) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (disc_compute_value_read(&cursor, &parsed[i])) return -1;
        indices[i] = -1;
        for (uint32_t j = 0; j < snapshot->count; j++)
            if (lmb_seg_id_equal(&parsed[i].lease_id,
                                 &snapshot->entries[j].owner.lease_id) &&
                parsed[i].fencing_epoch == snapshot->entries[j].owner.fencing_epoch)
                indices[i] = (int)j;
        if (indices[i] < 0) return -1;
        for (uint32_t j = 0; j < i; j++)
            if (indices[i] == indices[j]) return -1;
    }
    if (cursor.off != cursor.len) return -1;
    for (uint32_t i = 0; i < count; i++)
        snapshot->entries[indices[i]].compute = parsed[i];
    return 0;
}

int lmb_seg_advert_encode(const LmbSegAdvert *a,
                          uint8_t **body, uint32_t *body_len) {
    if (!lmb_seg_advert_valid(a)) return -1;
    LmbBuf b = {0};
    if (disc_buf_prefix(&b) || disc_buf_advert(&b, a)) {
        free(b.p); return -1;
    }
    return disc_finish(&b, body, body_len);
}

int lmb_seg_advert_decode(const void *body, size_t body_len, LmbSegAdvert *a) {
    if ((!body && body_len) || !a) return -1;
    memset(a, 0, sizeof *a);
    DiscCur c = { body, body_len, 0 };
    return disc_cur_prefix(&c) || disc_cur_advert(&c, a) || c.off != c.len ||
           !lmb_seg_advert_valid(a) ? -1 : 0;
}

static int disc_buf_query(LmbBuf *b, const LmbSegQuery *q) {
    return lmb_buf_str(b, q->model) ||
           lmb_buf_bytes(b, q->model_root, sizeof q->model_root) ||
           lmb_buf_bytes(b, q->tokenizer_root, sizeof q->tokenizer_root) ||
           lmb_buf_u32(b, q->layer_begin) || lmb_buf_u32(b, q->layer_end) ||
           lmb_buf_u32(b, q->context_tokens) || lmb_buf_u32(b, q->rows) ||
           lmb_buf_u32(b, q->state_dtype) || lmb_buf_u32(b, q->state_width) ||
           lmb_buf_u64(b, q->required_capabilities) ||
           lmb_buf_str(b, q->engine_id) || lmb_buf_str(b, q->state_schema) ||
           lmb_buf_str(b, q->numeric_class);
}

static int disc_cur_query(DiscCur *c, LmbSegQuery *q) {
    return disc_cur_str(c, q->model, sizeof q->model) ||
           disc_cur_bytes(c, q->model_root, sizeof q->model_root) ||
           disc_cur_bytes(c, q->tokenizer_root, sizeof q->tokenizer_root) ||
           disc_cur_u32(c, &q->layer_begin) || disc_cur_u32(c, &q->layer_end) ||
           disc_cur_u32(c, &q->context_tokens) || disc_cur_u32(c, &q->rows) ||
           disc_cur_u32(c, &q->state_dtype) || disc_cur_u32(c, &q->state_width) ||
           disc_cur_u64(c, &q->required_capabilities) ||
           disc_cur_str(c, q->engine_id, sizeof q->engine_id) ||
           disc_cur_str(c, q->state_schema, sizeof q->state_schema) ||
           disc_cur_str(c, q->numeric_class, sizeof q->numeric_class);
}

int lmb_seg_query_encode(const LmbSegQuery *q,
                         uint8_t **body, uint32_t *body_len) {
    if (!lmb_seg_query_valid(q)) return -1;
    LmbBuf b = {0};
    if (disc_buf_prefix(&b) || disc_buf_query(&b, q)) {
        free(b.p); return -1;
    }
    return disc_finish(&b, body, body_len);
}

int lmb_seg_query_decode(const void *body, size_t body_len, LmbSegQuery *q) {
    if ((!body && body_len) || !q) return -1;
    memset(q, 0, sizeof *q);
    DiscCur c = { body, body_len, 0 };
    return disc_cur_prefix(&c) || disc_cur_query(&c, q) || c.off != c.len ||
           !lmb_seg_query_valid(q) ? -1 : 0;
}

static int disc_buf_owner(LmbBuf *b, const LmbSegOwner *o) {
    return lmb_buf_bytes(b, o->lease_id.bytes, sizeof o->lease_id.bytes) ||
           lmb_buf_u64(b, o->fencing_epoch) ||
           lmb_buf_u64(b, o->route_generation);
}

static int disc_cur_owner(DiscCur *c, LmbSegOwner *o) {
    return disc_cur_bytes(c, o->lease_id.bytes, sizeof o->lease_id.bytes) ||
           disc_cur_u64(c, &o->fencing_epoch) ||
           disc_cur_u64(c, &o->route_generation);
}

int lmb_seg_route_encode(const LmbSegRouteSnapshot *s,
                         uint8_t **body, uint32_t *body_len) {
    if (!s || !s->route_generation || s->count > LMB_SEG_ROUTE_MAX) return -1;
    LmbBuf b = {0};
    int bad = disc_buf_prefix(&b) || lmb_buf_u64(&b, s->route_generation) ||
              lmb_buf_u32(&b, s->complete != 0) || lmb_buf_u32(&b, s->count);
    for (uint32_t i = 0; !bad && i < s->count; i++) {
        const LmbSegRouteEntry *e = &s->entries[i];
        if (!lmb_seg_advert_valid(&e->advert) ||
            lmb_seg_id_is_zero(&e->owner.lease_id) ||
            !e->owner.fencing_epoch ||
            e->owner.route_generation != s->route_generation ||
            !e->transport ||
            (e->transport & ~(LMB_SEG_TRANSPORT_DIRECT |
                              LMB_SEG_TRANSPORT_RELAY))) {
            bad = 1; break;
        }
        bad = disc_buf_advert(&b, &e->advert) ||
              disc_buf_owner(&b, &e->owner) ||
              lmb_buf_u32(&b, e->transport);
    }
    if (bad) { free(b.p); return -1; }
    return disc_finish(&b, body, body_len);
}

int lmb_seg_route_decode(const void *body, size_t body_len,
                         LmbSegRouteSnapshot *s) {
    if ((!body && body_len) || !s) return -1;
    memset(s, 0, sizeof *s);
    DiscCur c = { body, body_len, 0 };
    if (disc_cur_prefix(&c) || disc_cur_u64(&c, &s->route_generation) ||
        disc_cur_u32(&c, &s->complete) || disc_cur_u32(&c, &s->count) ||
        !s->route_generation || s->complete > 1 || s->count > LMB_SEG_ROUTE_MAX)
        return -1;
    for (uint32_t i = 0; i < s->count; i++) {
        LmbSegRouteEntry *e = &s->entries[i];
        if (disc_cur_advert(&c, &e->advert) ||
            disc_cur_owner(&c, &e->owner) ||
            disc_cur_u32(&c, &e->transport) ||
            !lmb_seg_advert_valid(&e->advert) ||
            lmb_seg_id_is_zero(&e->owner.lease_id) ||
            !e->owner.fencing_epoch ||
            e->owner.route_generation != s->route_generation ||
            !e->transport ||
            (e->transport & ~(LMB_SEG_TRANSPORT_DIRECT |
                              LMB_SEG_TRANSPORT_RELAY)))
            return -1;
    }
    if (c.off != c.len) return -1;
    s->fetched_at_ms = disc_now_ms();
    return 0;
}

int lmb_seg_registration_reply_encode(const LmbSegOwner *owner,
                                      uint8_t **body, uint32_t *body_len) {
    if (!owner || lmb_seg_id_is_zero(&owner->lease_id) ||
        !owner->fencing_epoch || !owner->route_generation) return -1;
    LmbBuf b = {0};
    if (disc_buf_prefix(&b) || disc_buf_owner(&b, owner)) {
        free(b.p); return -1;
    }
    return disc_finish(&b, body, body_len);
}

int lmb_seg_registration_reply_decode(const void *body, size_t body_len,
                                      LmbSegOwner *owner) {
    if ((!body && body_len) || !owner) return -1;
    memset(owner, 0, sizeof *owner);
    DiscCur c = { body, body_len, 0 };
    return disc_cur_prefix(&c) || disc_cur_owner(&c, owner) || c.off != c.len ||
           lmb_seg_id_is_zero(&owner->lease_id) || !owner->fencing_epoch ||
           !owner->route_generation ? -1 : 0;
}

/* An old tracker closes on an unknown op. Remember that result briefly so
 * a discovery worker does not repeat the probe on every refresh. */
typedef struct { char tracker[256]; uint64_t retry_at_ms; } DiscComputeLegacy;
static DiscComputeLegacy disc_compute_legacy[4];
static pthread_mutex_t disc_compute_legacy_lock = PTHREAD_MUTEX_INITIALIZER;

static int disc_compute_should_query(const char *tracker) {
    int allowed = 1;
    uint64_t now = disc_now_ms();
    pthread_mutex_lock(&disc_compute_legacy_lock);
    for (size_t i = 0; i < 4; i++)
        if (!strcmp(disc_compute_legacy[i].tracker, tracker) &&
            now < disc_compute_legacy[i].retry_at_ms) allowed = 0;
    pthread_mutex_unlock(&disc_compute_legacy_lock);
    return allowed;
}

static void disc_compute_mark_legacy(const char *tracker) {
    uint64_t now = disc_now_ms();
    pthread_mutex_lock(&disc_compute_legacy_lock);
    size_t slot = 0;
    for (size_t i = 0; i < 4; i++)
        if (!strcmp(disc_compute_legacy[i].tracker, tracker) ||
            !disc_compute_legacy[i].tracker[0]) { slot = i; break; }
    snprintf(disc_compute_legacy[slot].tracker,
             sizeof disc_compute_legacy[slot].tracker, "%s", tracker);
    disc_compute_legacy[slot].retry_at_ms = now + 60000u;
    pthread_mutex_unlock(&disc_compute_legacy_lock);
}

/* The ordinary tracker request inherits a five-minute I/O timeout. This
 * optional sideband must never hold a placement refresh for that long. */
static int disc_compute_query(const char *tracker, const void *body,
                              uint32_t body_len, LmbMsg *reply) {
    int fd = lmb_connect_ms_io(tracker, 2000, 2000);
    if (fd < 0) return -1;
    int rc = lmb_auth(fd);
    if (!rc) rc = lmb_send(fd, LMB_SEG_COMPUTE_GET, body, body_len, NULL, 0);
    if (!rc) rc = lmb_recv(fd, reply);
    lmb_close(fd);
    return rc;
}

int lmb_seg_compute_probe(const char *tracker) {
    if (!tracker || !*tracker) return 0;
    LmbSegRouteSnapshot empty = {0};
    uint8_t *body = NULL; uint32_t body_len = 0;
    if (lmb_seg_compute_get_encode(&empty, &body, &body_len)) return 0;
    LmbMsg reply = {0};
    int rc = disc_compute_query(tracker, body, body_len, &reply);
    free(body);
    int supported = !rc && reply.op == LMB_SEG_COMPUTE_GET_R &&
                    !reply.pay_len &&
                    !lmb_seg_compute_get_decode(reply.body, reply.body_len,
                                                &empty);
    lmb_msg_free(&reply);
    return supported;
}

int lmb_seg_routes_fetch(const char *tracker, const LmbSegQuery *query,
                         LmbSegRouteSnapshot *snapshot) {
    if (!tracker || !*tracker || !snapshot) return -1;
    /* Transport hooks are translation-unit local. This module owns the
     * tracker sockets, so encryption must be initialized here as well as in
     * the calling executable. */
    if (lmb_secure_init()) return -1;
    uint8_t *body = NULL; uint32_t body_len = 0;
    if (lmb_seg_query_encode(query, &body, &body_len)) return -1;
    LmbMsg reply = {0};
    int rc = lmb_request(tracker, LMB_SEG_ROUTES, body, body_len, &reply);
    free(body);
    if (rc || reply.op != LMB_SEG_ROUTES_R || reply.pay_len) {
        lmb_msg_free(&reply); return -1;
    }
    rc = lmb_seg_route_decode(reply.body, reply.body_len, snapshot);
    lmb_msg_free(&reply);
    if (!rc && snapshot->complete !=
        (uint32_t)lmb_seg_route_complete(snapshot, query)) return -1;
    if (rc || !snapshot->complete || !snapshot->count ||
        !disc_compute_should_query(tracker)) return rc;
    /* Telemetry is optional. A malformed, unsupported or unavailable sideband
     * must leave the valid v1 placement usable with its conservative ranking. */
    body = NULL; body_len = 0;
    if (lmb_seg_compute_get_encode(snapshot, &body, &body_len)) return rc;
    memset(&reply, 0, sizeof reply);
    int query_rc = disc_compute_query(tracker, body, body_len, &reply);
    free(body);
    if (!query_rc && reply.op == LMB_SEG_COMPUTE_GET_R && !reply.pay_len &&
        !lmb_seg_compute_get_decode(reply.body, reply.body_len, snapshot)) {
        uint64_t received = disc_now_ms();
        for (uint32_t i = 0; i < snapshot->count; i++)
            snapshot->entries[i].last_compute_ms =
                snapshot->entries[i].compute.samples ? received : UINT64_MAX;
    }
    else if (!query_rc && reply.op == LMB_ERR)
        disc_compute_mark_legacy(tracker);
    lmb_msg_free(&reply);
    return rc;
}

struct LmbSegDiscovery {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int stop;
    int have_snapshot;
    int refresh_requested; /* protected by lock, including during a fetch */
    uint32_t refresh_ms;
    char tracker[256];
    LmbSegQuery query;
    LmbSegRouteSnapshot snapshot;
    uint32_t probe_cursor;
};

static int disc_same_route(const LmbSegRouteEntry *a,
                           const LmbSegRouteEntry *b) {
    return !strcmp(a->advert.peer_name, b->advert.peer_name) &&
           !strcmp(a->advert.addr, b->advert.addr) &&
           a->advert.layer_begin == b->advert.layer_begin &&
           a->advert.layer_end == b->advert.layer_end;
}

static uint64_t disc_probe_us(const char *addr) {
    uint64_t before = disc_now_ms();
    int fd = lmb_connect_ms_io(addr, 250, 250);
    if (fd < 0) return 0;
    LmbMsg reply = {0};
    int bad = lmb_send(fd, LMB_PING, NULL, 0, NULL, 0) ||
              lmb_recv(fd, &reply) || reply.op != LMB_OK;
    lmb_msg_free(&reply);
    lmb_close(fd);
    if (bad) return 0;
    uint64_t elapsed = (disc_now_ms() - before) * 1000u;
    return elapsed ? elapsed : 1u;
}

static void disc_enrich_routes(LmbSegDiscovery *d,
                               LmbSegRouteSnapshot *next,
                               const LmbSegRouteSnapshot *previous,
                               int have_previous) {
    uint64_t now = disc_now_ms();
    uint32_t probe_budget = next->count < 8u ? next->count : 8u;
    for (uint32_t i = 0; i < next->count; i++) {
        LmbSegRouteEntry *entry = &next->entries[i];
        const LmbSegRouteEntry *old = NULL;
        if (have_previous)
            for (uint32_t j = 0; j < previous->count; j++)
                if (disc_same_route(entry, &previous->entries[j])) {
                    old = &previous->entries[j]; break;
                }
        if (old) {
            entry->latency = old->latency;
            entry->last_probe_ms = old->last_probe_ms;
            if (!entry->compute.samples && !entry->last_compute_ms &&
                old->last_compute_ms &&
                now >= old->last_compute_ms &&
                now - old->last_compute_ms <= 10000u &&
                lmb_seg_id_equal(&entry->owner.lease_id, &old->owner.lease_id) &&
                entry->owner.fencing_epoch == old->owner.fencing_epoch) {
                entry->compute = old->compute;
                entry->last_compute_ms = old->last_compute_ms;
            }
        } else {
            lmb_predict_init(&entry->latency,
                entry->transport & LMB_SEG_TRANSPORT_DIRECT ? 50000u : 120000u);
        }
    }
    for (uint32_t n = 0; n < probe_budget && next->count; n++) {
        uint32_t i = d->probe_cursor++ % next->count;
        LmbSegRouteEntry *entry = &next->entries[i];
        if (!(entry->transport & LMB_SEG_TRANSPORT_DIRECT)) continue;
        uint64_t sample = disc_probe_us(entry->advert.addr);
        if (sample) {
            lmb_predict_observe(&entry->latency, sample);
            entry->last_probe_ms = now;
        } else {
            lmb_predict_failure(&entry->latency, now);
        }
    }
    for (uint32_t i = 0; i < next->count; i++) {
        LmbSegRouteEntry *entry = &next->entries[i];
        uint32_t queue = entry->advert.queue_depth + entry->advert.inflight;
        entry->predicted_us = lmb_predict_score(&entry->latency, queue);
        if (!lmb_predict_available(&entry->latency, now))
            entry->predicted_us = UINT64_MAX / 4u;
    }
}

static void *disc_worker(void *arg) {
    LmbSegDiscovery *d = (LmbSegDiscovery *)arg;
    for (;;) {
        pthread_mutex_lock(&d->lock);
        int stopped=d->stop;
        d->refresh_requested=0;
        pthread_mutex_unlock(&d->lock);
        if (stopped) break;
        LmbSegRouteSnapshot next;
        if (!lmb_seg_routes_fetch(d->tracker, &d->query, &next)) {
            LmbSegRouteSnapshot previous;
            int have_previous;
            pthread_mutex_lock(&d->lock);
            previous = d->snapshot;
            have_previous = d->have_snapshot;
            pthread_mutex_unlock(&d->lock);
            disc_enrich_routes(d, &next, &previous, have_previous);
            pthread_mutex_lock(&d->lock);
            /* Generations fence placements. Equal generations still replace
             * telemetry; a lower generation can only be a stale tracker. */
            if (!d->have_snapshot ||
                next.route_generation >= d->snapshot.route_generation) {
                d->snapshot = next;
                d->have_snapshot = 1;
            }
            pthread_mutex_unlock(&d->lock);
        }
        pthread_mutex_lock(&d->lock);
        if (d->stop) { pthread_mutex_unlock(&d->lock); break; }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += d->refresh_ms / 1000u;
        until.tv_nsec += (long)(d->refresh_ms % 1000u) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++; until.tv_nsec -= 1000000000L;
        }
        while (!d->stop && !d->refresh_requested)
            if (pthread_cond_timedwait(&d->wake,&d->lock,&until)==ETIMEDOUT) break;
        int stop = d->stop;
        pthread_mutex_unlock(&d->lock);
        if (stop) break;
    }
    return NULL;
}

LmbSegDiscovery *lmb_seg_discovery_start(const char *tracker,
                                         const LmbSegQuery *query,
                                         uint32_t refresh_ms) {
    if (!tracker || !*tracker || strlen(tracker) >= 256 ||
        !lmb_seg_query_valid(query) || refresh_ms < 50 || refresh_ms > 3600000)
        return NULL;
    LmbSegDiscovery *d = (LmbSegDiscovery *)calloc(1, sizeof *d);
    if (!d) return NULL;
    snprintf(d->tracker, sizeof d->tracker, "%s", tracker);
    d->query = *query; d->refresh_ms = refresh_ms;
    if (pthread_mutex_init(&d->lock, NULL)) { free(d); return NULL; }
    if (pthread_cond_init(&d->wake, NULL)) {
        pthread_mutex_destroy(&d->lock); free(d); return NULL;
    }
    if (pthread_create(&d->thread, NULL, disc_worker, d)) {
        pthread_cond_destroy(&d->wake); pthread_mutex_destroy(&d->lock);
        free(d); return NULL;
    }
    return d;
}

int lmb_seg_discovery_snapshot(LmbSegDiscovery *d,
                               LmbSegRouteSnapshot *snapshot) {
    if (!d || !snapshot) return -1;
    pthread_mutex_lock(&d->lock);
    int have = d->have_snapshot;
    if (have) *snapshot = d->snapshot;
    pthread_mutex_unlock(&d->lock);
    return have;
}

int lmb_seg_discovery_request_refresh(LmbSegDiscovery *d) {
    if (!d) return -1;
    pthread_mutex_lock(&d->lock);
    int stopped=d->stop;
    if (!stopped) {
        d->refresh_requested=1;
        pthread_cond_signal(&d->wake);
    }
    pthread_mutex_unlock(&d->lock);
    return stopped ? -1 : 0;
}

int lmb_seg_discovery_refresh_now(LmbSegDiscovery *d,
                                  LmbSegRouteSnapshot *snapshot) {
    if (!d || !snapshot) return -1;
    LmbSegRouteSnapshot next;
    if (lmb_seg_routes_fetch(d->tracker, &d->query, &next)) return -1;

    /* A failover must not wait for direct-connect probes. Preserve observations
     * for routes already known by the worker and initialize new ones with the
     * transport prior; the next background pass resumes normal probing. */
    pthread_mutex_lock(&d->lock);
    uint64_t now = disc_now_ms();
    for (uint32_t i = 0; i < next.count; i++) {
        LmbSegRouteEntry *entry = &next.entries[i];
        const LmbSegRouteEntry *old = NULL;
        if (d->have_snapshot)
            for (uint32_t j = 0; j < d->snapshot.count; j++)
                if (disc_same_route(entry, &d->snapshot.entries[j])) {
                    old = &d->snapshot.entries[j];
                    break;
                }
        if (old) {
            entry->latency = old->latency;
            entry->last_probe_ms = old->last_probe_ms;
            if (!entry->compute.samples && !entry->last_compute_ms &&
                old->last_compute_ms &&
                now >= old->last_compute_ms &&
                now - old->last_compute_ms <= 10000u &&
                lmb_seg_id_equal(&entry->owner.lease_id, &old->owner.lease_id) &&
                entry->owner.fencing_epoch == old->owner.fencing_epoch) {
                entry->compute = old->compute;
                entry->last_compute_ms = old->last_compute_ms;
            }
        } else {
            lmb_predict_init(&entry->latency,
                entry->transport & LMB_SEG_TRANSPORT_DIRECT ? 50000u : 120000u);
        }
        uint32_t queue = entry->advert.queue_depth + entry->advert.inflight;
        entry->predicted_us = lmb_predict_score(&entry->latency, queue);
        if (!lmb_predict_available(&entry->latency, now))
            entry->predicted_us = UINT64_MAX / 4u;
    }
    if (!d->have_snapshot ||
        next.route_generation >= d->snapshot.route_generation) {
        d->snapshot = next;
        d->have_snapshot = 1;
    }
    *snapshot = d->snapshot;
    pthread_mutex_unlock(&d->lock);
    return 1;
}

void lmb_seg_discovery_stop(LmbSegDiscovery *d) {
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    d->stop = 1;
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->lock);
    pthread_join(d->thread, NULL);
    pthread_cond_destroy(&d->wake); pthread_mutex_destroy(&d->lock);
    free(d);
}
