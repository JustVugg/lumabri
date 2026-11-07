/* Keep upstream's complete state/chaining checks, with an explicit budget:
 * resident preparation must not use its ordinary unlimited-cache default. */
#include "segment_runtime.h"
static int bounded_open(const char *family,const ColiSegmentEngineOptions *options,
                        ColiSegmentEngine **engine,char *error,size_t size) {
    ColiSegmentEngineOptions bounded=*options;
    if (!bounded.memory_limit_bytes) bounded.memory_limit_bytes=1ull<<30;
    return coli_segment_engine_open(family,&bounded,engine,error,size);
}
#define coli_segment_engine_open bounded_open
#include "tests/test_segment_adapters_real.c"
#undef coli_segment_engine_open
