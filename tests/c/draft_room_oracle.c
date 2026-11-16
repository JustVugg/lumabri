/* Provisional rows at the adapter: what a rollback leaves behind is, bit for
 * bit, what plain decode builds, and a block never needs more memory than
 * the session's limit covers. Runs on the reduced fixture of a family whose
 * rollback keeps state copies (Qwen3.8). */
#define main upstream_state_test_main
#include "tests/test_segment_adapters_real.c"
#undef main
#include "edge_runtime.h"
#include "edge_adapters.h"
#include "engine_patches/segment_draft.h"
#include <assert.h>
#ifndef LMB_DRAFT_FAMILY
#define LMB_DRAFT_FAMILY "qwen38"
#endif
#define OK(x) do {if((x)){fprintf(stderr,"line %d: %s\n",__LINE__,error);abort();}}while(0)

static int provisional(ColiSegmentSession *session,uint64_t position,uint32_t rows,uint32_t width,
                       const float *input,float *output,const int32_t *tokens,char *error,size_t size) {
    ColiSegmentRunRequest request={.struct_size=sizeof request,.rows=rows,.position=position,
        .token_ids=tokens,.token_count=rows,.input=input,.input_bytes=(size_t)rows*width*sizeof(float),
        .output=output,.output_bytes=(size_t)rows*width*sizeof(float),
        .reserved_u64={LMB_SEGMENT_RUN_PROVISIONAL}};
    return coli_segment_run(session,&request,error,size);
}

/* Smallest limit at which the session may carry `rows` rows per block. */
static uint64_t smallest_limit(ColiSegmentSession *session,uint32_t rows) {
    uint64_t low=1,high=1ull<<40;uint32_t got=0;
    assert(!lmb_segment_session_draft_room(session,high,&got)&&got>=rows);
    while(low<high) {
        uint64_t middle=low+(high-low)/2;
        assert(!lmb_segment_session_draft_room(session,middle,&got));
        if(got>=rows)high=middle;else low=middle+1;
    }
    return low;
}

int main(int argc,char **argv) {
    if(argc!=2)return 2;
    char error[512]="draft state mismatch";
    OK(register_all());OK(coli_qwen38_edge_adapter_register());OK(coli_olmoe_edge_adapter_register());
    ColiEdgeEngine *edge=NULL;ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=argv[1],.memory_limit_bytes=1ull<<30};
    OK(coli_edge_engine_open(LMB_DRAFT_FAMILY,&eo,&edge,error,sizeof error));
    ColiEdgeCapabilities ec={.struct_size=sizeof ec};OK(coli_edge_engine_capabilities(edge,&ec,error,sizeof error));
    uint32_t width=ec.state_width;size_t row=(size_t)width*sizeof(float);
    int32_t tokens[24],wrong[24];
    for(int i=0;i<24;i++){tokens[i]=1+i%17;wrong[i]=40+i;}
    float *input=malloc(24*row),*bad=malloc(24*row),*plain=malloc(24*row),*draft=malloc(24*row);
    assert(input&&bad&&plain&&draft);
    ColiEdgeEmbedRequest er={.struct_size=sizeof er,.rows=24,.token_ids=tokens,.token_count=24,.output=input,.output_bytes=24*row};
    OK(coli_edge_embed(edge,&er,error,sizeof error));
    er.token_ids=wrong;er.output=bad;OK(coli_edge_embed(edge,&er,error,sizeof error));

    ColiSegmentEngine *engine=NULL;ColiSegmentCapabilities caps;
    OK(open_engine_limited(LMB_DRAFT_FAMILY,argv[1],0,ec.num_layers,64,1ull<<30,&engine,&caps,error,sizeof error));
    assert(lmb_segment_engine_draft_rows(engine));
    ColiSegmentSession *a=NULL,*b=NULL;
    OK(create_session(engine,64,&a,error,sizeof error));OK(create_session(engine,64,&b,error,sizeof error));

    /* Plain decode: a prefill of 8 rows, then one row at a time. */
    OK(run_rows(a,0,8,width,input,plain,tokens,error,sizeof error));
    for(uint32_t at=8;at<16;at++)
        OK(run_rows(a,at,1,width,input+(size_t)at*width,plain+(size_t)at*width,tokens+at,error,sizeof error));

    /* The same tokens through blocks whose later rows were wrong drafts. */
    uint32_t rows=0;
    assert(!lmb_segment_session_draft_room(b,0,&rows)&&rows==LMB_SEGMENT_DRAFT_MAX_ROWS);
    OK(run_rows(b,0,8,width,input,draft,tokens,error,sizeof error));
    float *block=malloc(8*row);int32_t ids[8];assert(block);
    /* Block at 8: rows 8, 9 right, three wrong rows after them. */
    memcpy(block,input+8u*width,2*row);memcpy(block+2u*width,bad+10u*width,3*row);
    memcpy(ids,tokens+8,2*sizeof *ids);memcpy(ids+2,wrong+10,3*sizeof *ids);
    OK(provisional(b,8,5,width,block,draft+8u*width,ids,error,sizeof error));
    assert(!memcmp(draft+8u*width,plain+8u*width,2*row));
    assert(memcmp(draft+10u*width,plain+10u*width,row));
    /* Not into the prefill, not forwards, not onto a row that has no copy. */
    assert(lmb_segment_session_rewind(b,7,error,sizeof error));
    assert(lmb_segment_session_rewind(b,14,error,sizeof error));
    OK(lmb_segment_session_rewind(b,10,error,sizeof error));
    /* Block at 10: one right row, then wrong ones; roll back twice. */
    memcpy(block,input+10u*width,row);memcpy(block+width,bad+11u*width,2*row);
    ids[0]=tokens[10];memcpy(ids+1,wrong+11,2*sizeof *ids);
    OK(provisional(b,10,3,width,block,draft+10u*width,ids,error,sizeof error));
    OK(lmb_segment_session_rewind(b,12,error,sizeof error));
    OK(lmb_segment_session_rewind(b,11,error,sizeof error));
    /* Block at 11: all right, kept whole. */
    OK(provisional(b,11,5,width,input+11u*width,draft+11u*width,tokens+11,error,sizeof error));
    OK(lmb_segment_session_rewind(b,16,error,sizeof error));
    assert(!memcmp(draft+8u*width,plain+8u*width,8*row));
    MemoryStream sa={0},sb={0};
    OK(coli_segment_snapshot(a,memory_write,&sa,error,sizeof error));
    OK(coli_segment_snapshot(b,memory_write,&sb,error,sizeof error));
    assert(sa.size==sb.size&&!memcmp(sa.data,sb.data,sa.size));
    /* A plain run closes the window. */
    OK(run_rows(b,16,1,width,input+16u*width,draft+16u*width,tokens+16,error,sizeof error));
    assert(lmb_segment_session_rewind(b,16,error,sizeof error));

    /* Memory: each row beyond the first costs one state copy, and the
     * session refuses a block its limit does not cover. */
    assert(!lmb_segment_session_draft_room(b,1,&rows)&&rows==0);
    uint64_t two=smallest_limit(b,2),three=smallest_limit(b,3);
    uint64_t copy=three-two;
    if(!strcmp(LMB_DRAFT_FAMILY,"qwen38")) {
        assert(copy>0&&two>copy);
        assert(!lmb_segment_session_draft_room(b,two-1,&rows)&&rows==0);
        assert(!lmb_segment_session_draft_room(b,two+5*copy,&rows)&&rows==7);
        assert(!lmb_segment_session_draft_room(b,two+5*copy-1,&rows)&&rows==6);
        assert(!lmb_segment_session_draft_room(b,two+100*copy,&rows)&&rows==LMB_SEGMENT_DRAFT_MAX_ROWS);
        assert(!lmb_segment_session_draft_room(b,three,&rows)&&rows==3);
        free(sb.data);memset(&sb,0,sizeof sb);MemoryStream after={0};
        OK(coli_segment_snapshot(b,memory_write,&sb,error,sizeof error));
        assert(provisional(b,17,4,width,input+17u*width,draft+17u*width,tokens+17,error,sizeof error));
        OK(coli_segment_snapshot(b,memory_write,&after,error,sizeof error));
        assert(sb.size==after.size&&!memcmp(sb.data,after.data,sb.size));free(after.data);
        OK(provisional(b,17,3,width,input+17u*width,draft+17u*width,tokens+17,error,sizeof error));
        printf("%s DRAFT ROOM: PASS (rollback state bit-identical to plain decode; %llu bytes per extra row, "
               "blocks beyond the session limit refused)\n",LMB_DRAFT_FAMILY,(unsigned long long)copy);
    } else {
        assert(two==1&&three==1);
        printf("%s DRAFT ROOM: PASS (rollback state bit-identical to plain decode; no memory beyond the session state)\n",LMB_DRAFT_FAMILY);
    }
    free(sa.data);free(sb.data);free(block);free(input);free(bad);free(plain);free(draft);
    coli_segment_session_destroy(a);coli_segment_session_destroy(b);
    OK(coli_segment_engine_close(engine,error,sizeof error));
    return 0;
}
