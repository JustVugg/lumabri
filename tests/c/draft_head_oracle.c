/* The draft head at the Edge (Qwen3.8 MTP), without a network.
 *
 *   draft-head-oracle MODEL_DIR RESIDENT STATE   a checkpoint with a head; the planner's Edge
 *                                                weights and one 64-token session, in bytes
 *   draft-head-oracle MODEL_DIR none             a checkpoint without one
 *
 * With LUMABRI_RESIDENCY_POLICY=engine-locked every engine allocation is
 * counted, and the counts are held against the planner's prices.
 *
 * The head loads only when the caller asks for it, stays inside the memory
 * the planner priced, and keeps its rows per conversation: what one
 * conversation is proposed never depends on another, on how its rows were
 * chunked, or on the tail rows earlier proposals left behind. */
#define main upstream_state_test_main
#include "tests/test_segment_adapters_real.c"
#undef main
#include "edge_runtime.h"
#include "edge_adapters.h"
#include "src/runtime/lumabri_draft_head.h"
#include "src/runtime/lumabri_engine_memory.h"
#include <assert.h>
#define OK(x) do {if((x)){fprintf(stderr,"line %d: %s\n",__LINE__,error);abort();}}while(0)
#define CONTEXT 64u

static uint64_t open_edge(const char *dir,uint64_t flag,ColiEdgeEngine **edge,ColiEdgeCapabilities *cap) {
    char error[512]="";
    ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=dir,.memory_limit_bytes=1ull<<30,.reserved_u64={flag}};
    OK(coli_edge_engine_open("qwen38",&eo,edge,error,sizeof error));
    memset(cap,0,sizeof *cap);cap->struct_size=sizeof *cap;
    OK(coli_edge_engine_capabilities(*edge,cap,error,sizeof error));
    return cap->resident_bytes;
}

/* Proposals after `token` fed at `position`, as a count and three slots. */
typedef struct { uint32_t count; int32_t ids[LMB_DRAFT_HEAD_MAX]; } Proposal;
static Proposal propose(const LmbDraftHeadBinding *h,void *s,int32_t token,uint64_t position) {
    Proposal p={0};
    p.count=h->propose(h->owner,s,token,position,p.ids,LMB_DRAFT_HEAD_MAX);
    return p;
}
static int same(Proposal a,Proposal b) {
    return a.count==b.count && !memcmp(a.ids,b.ids,a.count*sizeof *a.ids);
}

static uint64_t engine_bytes(void) {
    LmbEngineMemory memory;lmb_engine_memory_snapshot(&memory);
    assert(!memory.fault);
    return memory.enabled?memory.requested_bytes:0;
}

int main(int argc,char **argv) {
    if(argc!=3 && argc!=4)return 2;
    char error[512]="draft head mismatch";
    OK(register_all());OK(coli_qwen38_edge_adapter_register());
    ColiEdgeEngine *edge=NULL;ColiEdgeCapabilities cap;

    /* Not asked for: the Edge is exactly the Edge it was. */
    uint64_t plain=open_edge(argv[1],0,&edge,&cap);
    assert(!lmb_draft_head());
    coli_edge_engine_close(edge);edge=NULL;
    uint64_t idle=engine_bytes();

    uint64_t with=open_edge(argv[1],LMB_EDGE_OPEN_DRAFT_HEAD,&edge,&cap);
    const LmbDraftHeadBinding *h=lmb_draft_head();
    if(!strcmp(argv[2],"none")) {
        assert(!h && with==plain);
        coli_edge_engine_close(edge);
        puts("Qwen3.8 draft head: a checkpoint without one opens unchanged PASS");
        return 0;
    }
    if(argc!=4)return 2;
    uint64_t planned=strtoull(argv[2],NULL,10),planned_state=strtoull(argv[3],NULL,10);
    assert(h && with>plain && planned && planned_state && with<=planned);
    /* Every byte the Edge and its head allocated, against the planner's price. */
    uint64_t loaded=engine_bytes();
    int counted=loaded!=0;
    assert(!counted || (loaded>idle && loaded-idle<=planned));

    uint32_t width=cap.state_width;size_t row=(size_t)width*sizeof(float);
    int32_t ta[24],tb[24];
    float *ra=malloc(24*row),*rb=malloc(24*row);assert(ra&&rb);
    /* Any rows do: the head only reads them. Deterministic, different per conversation. */
    for(int i=0;i<24;i++){ta[i]=1+i%17;tb[i]=60+i%11;}
    for(size_t i=0;i<24u*width;i++){ra[i]=(float)((i*2654435761u>>8)%2001)/1000.f-1.f;rb[i]=(float)((i*40503u>>4)%2001)/1000.f-1.f;}

    void *a=NULL,*b=NULL,*c=NULL,*d=NULL;
    assert(!h->session_create(h->owner,CONTEXT,&a));
    assert(!counted || engine_bytes()-loaded<=planned_state);
    assert(!h->session_create(h->owner,CONTEXT,&b));
    assert(!h->session_create(h->owner,CONTEXT,&c)&&!h->session_create(h->owner,CONTEXT,&d));
    assert(h->session_create(h->owner,0,&(void*){NULL}));

    /* A alone: prefill of 8 rows, a proposal, two committed rows, a proposal. */
    assert(!h->commit(h->owner,a,0,8,ta,ra));
    Proposal a8=propose(h,a,ta[8],8);
    assert(a8.count==LMB_DRAFT_HEAD_MAX);
    for(uint32_t i=0;i<a8.count;i++)assert(a8.ids[i]>=0&&(uint32_t)a8.ids[i]<cap.vocab_size);
    /* The pending row was consumed: a second question at the same place is not answered twice. */
    assert(!propose(h,a,ta[8],8).count);
    assert(!h->commit(h->owner,a,8,2,ta+8,ra+8u*width));
    Proposal a10=propose(h,a,ta[10],10);
    assert(a10.count==LMB_DRAFT_HEAD_MAX);

    /* B has other rows and other tokens; C repeats A while B runs in between,
     * with the prefill cut in two. */
    assert(!h->commit(h->owner,c,0,5,ta,ra));
    assert(!h->commit(h->owner,b,0,8,tb,rb));
    assert(!h->commit(h->owner,c,5,3,ta+5,ra+5u*width));
    Proposal b8=propose(h,b,tb[8],8),c8=propose(h,c,ta[8],8);
    assert(b8.count==LMB_DRAFT_HEAD_MAX && same(a8,c8) && !same(a8,b8));
    assert(!h->commit(h->owner,b,8,3,tb+8,rb+8u*width));
    assert(!h->commit(h->owner,c,8,2,ta+8,ra+8u*width));
    assert(same(a10,propose(h,c,ta[10],10)));

    /* D never asks at 8, so no deeper draft ever wrote a tail row there: the
     * rows A's proposals left past its settled ones changed nothing. */
    assert(!h->commit(h->owner,d,0,8,ta,ra)&&!h->commit(h->owner,d,8,2,ta+8,ra+8u*width));
    assert(same(a10,propose(h,d,ta[10],10)));

    /* A position that neither continues nor restarts silences the head ... */
    assert(!h->commit(h->owner,d,20,1,ta+20,ra+20u*width));
    assert(!propose(h,d,ta[21],21).count);
    /* ... until the conversation starts again at 0. */
    assert(!h->commit(h->owner,d,0,8,ta,ra));
    assert(same(a8,propose(h,d,ta[8],8)));

    /* Bounds: outside the context, outside the vocabulary. */
    assert(h->commit(h->owner,a,CONTEXT,1,ta,ra)&&h->commit(h->owner,a,CONTEXT-1,2,ta,ra));
    int32_t bad=(int32_t)cap.vocab_size;
    assert(h->commit(h->owner,a,12,1,&bad,ra)&&!propose(h,a,bad,12).count&&!propose(h,a,1,CONTEXT).count);
    /* The last position can take one proposal, never a tail row past the context. */
    void *e=NULL;assert(!h->session_create(h->owner,10,&e));
    assert(!h->commit(h->owner,e,0,9,ta,ra));
    assert(propose(h,e,ta[9],9).count==1);

    h->session_destroy(h->owner,a);h->session_destroy(h->owner,b);h->session_destroy(h->owner,c);
    h->session_destroy(h->owner,d);h->session_destroy(h->owner,e);
    coli_edge_engine_close(edge);
    assert(!lmb_draft_head());
    /* Closing gives back everything the head and its conversations held. */
    assert(!counted || engine_bytes()==idle);
    if(counted)printf("Qwen3.8 draft head: %llu bytes allocated of %llu planned, all returned on close; ",
                      (unsigned long long)(loaded-idle),(unsigned long long)planned);
    else printf("Qwen3.8 draft head: allocations not counted in this run (%llu planned); ",(unsigned long long)planned);
    puts("conversations independent, chunking and earlier proposals leave no trace, a gap silences it PASS");
    free(ra);free(rb);return 0;
}
