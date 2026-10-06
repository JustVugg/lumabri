/* Verify a replay against bytes already delivered. Frame boundaries, UTF-8
 * boundaries and tokenizer output chunks may differ; the visible prefix may
 * not. No new byte is emitted from a chunk until its overlapping prefix has
 * matched in full. This buffer lives only for one bounded HTTP request. */
#ifndef LMB_REPLAY_PREFIX_H
#define LMB_REPLAY_PREFIX_H
#include <stdlib.h>
#include <string.h>
typedef struct {
    unsigned char *bytes;
    size_t length,capacity,limit,matched,target;
    int mismatch;
} LmbReplayPrefix;
static inline void lmb_replay_begin(LmbReplayPrefix *r) { r->matched=0; r->target=r->length; r->mismatch=0; }
static inline int lmb_replay_feed(LmbReplayPrefix *r, const unsigned char *p, size_t n,
    int (*emit)(void *,const unsigned char *,size_t), void *opaque) {
    if (!r || (!p && n) || !emit || r->mismatch || r->matched>r->target || r->target>r->length) return -1;
    if (!n) return 0;
    size_t overlap=r->target-r->matched; if (overlap>n) overlap=n;
    if (overlap && memcmp(r->bytes+r->matched,p,overlap)) { r->mismatch=1; return -1; }
    r->matched+=overlap; p+=overlap; n-=overlap;
    if (!n) return 0;
    if (r->length>r->limit || n>r->limit-r->length) return -1;
    if (r->length+n>r->capacity) {
        size_t capacity=r->capacity ? r->capacity : 16384;
        if (capacity>r->limit) capacity=r->limit;
        while (capacity<r->length+n) capacity=capacity>r->limit/2 ? r->limit : capacity*2;
        unsigned char *bytes=realloc(r->bytes,capacity); if (!bytes) return -1;
        r->bytes=bytes; r->capacity=capacity;
    }
    if (emit(opaque,p,n)) return -1;
    memcpy(r->bytes+r->length,p,n); r->length+=n; return 0;
}
static inline int lmb_replay_complete(const LmbReplayPrefix *r) {
    return r && !r->mismatch && r->matched==r->target;
}
static inline void lmb_replay_free(LmbReplayPrefix *r) {
    if (!r) return;
    /* Transcripts are request-private, not telemetry or calibration data. */
    if (r->bytes) { volatile unsigned char *p=r->bytes; for (size_t i=0;i<r->length;i++) p[i]=0; }
    free(r->bytes); memset(r,0,sizeof *r);
}
#endif
