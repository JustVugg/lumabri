#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "src/runtime/lumabri_replay_prefix.h"
#include "src/runtime/lumabri_request_seed.h"
typedef struct { unsigned char bytes[128]; size_t n; int fail; } Output;
static int emit(void *opaque, const unsigned char *p, size_t n) {
    Output *o=opaque; if (o->fail) return -1;
    assert(n<=sizeof o->bytes-o->n); memcpy(o->bytes+o->n,p,n); o->n+=n; return 0;
}
int main(void) {
    const unsigned char text[]="A\0B\xc3\xa8\nDATA 7 1\n!";
    for (size_t prefix=0;prefix<sizeof text;prefix++) for (size_t chunk=1;chunk<sizeof text;chunk++) {
        LmbReplayPrefix r={.limit=sizeof text}; Output out={0};
        assert(!lmb_replay_feed(&r,NULL,0,emit,&out));
        assert(!lmb_replay_feed(&r,text,prefix,emit,&out)); lmb_replay_begin(&r);
        assert(lmb_replay_complete(&r)==!prefix);
        for (size_t at=0;at<sizeof text;) {
            size_t n=sizeof text-at; if (n>chunk) n=chunk;
            assert(!lmb_replay_feed(&r,text+at,n,emit,&out)); at+=n;
        }
        assert(lmb_replay_complete(&r) && out.n==sizeof text && !memcmp(out.bytes,text,sizeof text));
        /* A third attempt verifies everything emitted by the second as well. */
        lmb_replay_begin(&r); assert(!lmb_replay_feed(&r,text,sizeof text,emit,&out));
        assert(lmb_replay_complete(&r) && out.n==sizeof text); lmb_replay_free(&r);
    }
    LmbReplayPrefix r={.limit=8}; Output out={0};
    assert(!lmb_replay_feed(&r,(const unsigned char *)"abc",3,emit,&out)); lmb_replay_begin(&r);
    assert(lmb_replay_feed(&r,(const unsigned char *)"abXdef",6,emit,&out));
    assert(r.mismatch && !lmb_replay_complete(&r) && out.n==3);
    lmb_replay_free(&r); r.limit=8; out=(Output){0};
    assert(!lmb_replay_feed(&r,(const unsigned char *)"abc",3,emit,&out)); lmb_replay_begin(&r);
    assert(!lmb_replay_feed(&r,(const unsigned char *)"ab",2,emit,&out)); assert(!lmb_replay_complete(&r));
    assert(lmb_replay_feed(&r,(const unsigned char *)"c123456",7,emit,&out)); assert(out.n==3);
    lmb_replay_free(&r); r.limit=8; out=(Output){.fail=1};
    assert(lmb_replay_feed(&r,text,1,emit,&out) && r.length==0); lmb_replay_free(&r);
    uint64_t seed=0;
    assert(!lmb_request_seed("0",&seed) && !seed);
    assert(!lmb_request_seed("18446744073709551615",&seed) && seed==UINT64_MAX);
    const char *bad[]={"","-1","+1","18446744073709551616","000000000000000000000","1x","1 2"," 1"};
    for (size_t i=0;i<sizeof bad/sizeof *bad;i++) assert(lmb_request_seed(bad[i],&seed));
    puts("REPLAY PREFIX: PASS (all boundaries, repeated recovery, NUL/UTF-8, divergent/short replay, limits and cancellation)");
    return 0;
}
