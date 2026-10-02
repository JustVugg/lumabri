/* Exercise the real public Colibri ABI around Lumabri's experimental wrapper.
 * Product registration and bounded admission remain disabled independently. */
#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
static int weights_sealed;
static unsigned forbidden_reads;
static ssize_t guarded_pread(int fd,void *p,size_t n,off_t off) {
    if(weights_sealed){forbidden_reads++;errno=EIO;return -1;}return pread(fd,p,n,off);
}
#define pread guarded_pread
#define main upstream_cli_main
#include "v41_range_core.c"
#undef main
#undef pread
#include "engine_patches/v41_abi_lab.h"

static int cancelled(void *p){(void)p;return 1;}
static void prove(const char *path,int cut) {
    char error[512]={0};
#define OK(expr) do {int rc=(expr);if(rc){fprintf(stderr,"ABI failure at %d: %s\n",__LINE__,error);abort();}}while(0)
    OK(lmb_v41_lab_register());
    ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=path,.backend_mask=COLI_EDGE_CAP_CPU};
    ColiEdgeEngine *edge=NULL;
    eo.memory_limit_bytes=1;assert(coli_edge_engine_open("deepseek_v41",&eo,&edge,error,sizeof error) && !edge);
    eo.memory_limit_bytes=0;OK(coli_edge_engine_open("deepseek_v41",&eo,&edge,error,sizeof error));
    ColiEdgeCapabilities ec={.struct_size=sizeof ec};OK(coli_edge_engine_capabilities(edge,&ec,error,sizeof error));
    size_t tokens=0;OK(coli_edge_tokenize(edge,"Hello",5,NULL,0,&tokens,error,sizeof error));assert(tokens);
    int32_t *encoded=calloc(tokens,sizeof *encoded);assert(encoded);
    OK(coli_edge_tokenize(edge,"Hello",5,encoded,tokens,&tokens,error,sizeof error));
    size_t text_bytes=0;OK(coli_edge_detokenize(edge,encoded,tokens,NULL,0,&text_bytes,error,sizeof error));
    char *decoded=malloc(text_bytes+1);assert(decoded);
    OK(coli_edge_detokenize(edge,encoded,tokens,decoded,text_bytes+1,&text_bytes,error,sizeof error));
    assert(text_bytes==5 && !strcmp(decoded,"Hello"));free(encoded);free(decoded);
    ColiSegmentEngine *engines[2]={0};ColiSegmentSession *sessions[2][2]={{0}};
    for(int peer=0;peer<2;peer++) {
        ColiSegmentEngineOptions o={.struct_size=sizeof o,.model_dir=path,.layer_begin=peer ? (uint32_t)cut : 0,
            .layer_end=peer ? ec.num_layers : (uint32_t)cut,.context_tokens=96,.backend_mask=COLI_SEGMENT_CAP_CPU};
        o.memory_limit_bytes=1;assert(coli_segment_engine_open("deepseek_v41",&o,&engines[peer],error,sizeof error) && !engines[peer]);
        o.memory_limit_bytes=0;OK(coli_segment_engine_open("deepseek_v41",&o,&engines[peer],error,sizeof error));
        ColiSegmentCapabilities sc={.struct_size=sizeof sc};OK(coli_segment_engine_capabilities(engines[peer],&sc,error,sizeof error));
        assert(sc.max_context_tokens==96 && sc.state_width==ec.state_width && sc.max_batch_rows==1);
        assert(!strcmp(sc.state_schema,ec.state_schema) && !strcmp(sc.numeric_class,ec.numeric_class));
        assert(!(sc.flags&COLI_SEGMENT_CAP_SNAPSHOT));
        for(int chat=0;chat<2;chat++) {
            ColiSegmentSessionOptions so={.struct_size=sizeof so,.context_tokens=80};
            OK(coli_segment_session_create(engines[peer],&so,&sessions[peer][chat],error,sizeof error));
        }
        assert(coli_segment_engine_close(engines[peer],error,sizeof error)); /* live sessions own their engine */
    }
    Model reference={0},refs[2]={0};Cfg c;cfg_load(&c,path);
    model_load(&reference,path,c.n_routed,32);
    for(int l=0;l<c.n_layers;l++)for(int e=0;e<c.n_routed;e++)(void)expert_slot_at(&reference,&reference.cache[l],"layers",l,e);
    for(int i=0;i<2;i++)assert(!lmb_v41_session_model(&refs[i],&reference));
    size_t width=ec.state_width,bytes=width*4,aux=lmb_v41_state_aux(&c),auxbytes=(width-aux)*4;
    float *a=malloc(bytes),*b=malloc(bytes),*feedback[2]={calloc(1,auxbytes),calloc(1,auxbytes)};
    float *expected=malloc((size_t)c.vocab*4),*actual=malloc((size_t)c.vocab*4);
    assert(a && b && feedback[0] && feedback[1] && expected && actual);
    int32_t ids[2]={177,28};weights_sealed=1;
    for(int step=0;step<48;step++)for(int chat=0;chat<2;chat++) {
        ColiEdgeEmbedRequest embed={.struct_size=sizeof embed,.rows=1,.token_ids=&ids[chat],.token_count=1,
            .output=a,.output_bytes=bytes};
        OK(coli_edge_embed(edge,&embed,error,sizeof error));
        if(step)memcpy(a+aux,feedback[chat],auxbytes);
        for(int peer=0;peer<2;peer++) {
            ColiSegmentRunRequest run={.struct_size=sizeof run,.rows=1,.position=(uint64_t)step,
                .token_ids=&ids[chat],.token_count=1,.input=a,.input_bytes=bytes,.output=b,.output_bytes=bytes};
            if(step==8) {
                run.should_cancel=cancelled;assert(coli_segment_run(sessions[peer][chat],&run,error,sizeof error));run.should_cancel=NULL;
                run.position++;assert(coli_segment_run(sessions[peer][chat],&run,error,sizeof error));run.position--;
                float saved=a[0];a[0]=NAN;assert(coli_segment_run(sessions[peer][chat],&run,error,sizeof error));a[0]=saved;
                saved=a[2];a[2]=NAN;assert(coli_segment_run(sessions[peer][chat],&run,error,sizeof error));a[2]=saved;
                saved=a[aux];a[aux]=NAN;assert(coli_segment_run(sessions[peer][chat],&run,error,sizeof error));a[aux]=saved;
                run.input_bytes-=4;assert(coli_segment_run(sessions[peer][chat],&run,error,sizeof error));run.input_bytes+=4;
            }
            OK(coli_segment_run(sessions[peer][chat],&run,error,sizeof error));float *swap=a;a=b;b=swap;
        }
        memcpy(feedback[chat],a+aux,auxbytes);
        ColiEdgeLogitsRequest logits={.struct_size=sizeof logits,.rows=1,.input=a,.input_bytes=bytes,
            .logits=actual,.logits_capacity=(size_t)c.vocab};OK(coli_edge_logits(edge,&logits,error,sizeof error));
        forward(&refs[chat],&ids[chat],1,expected);assert(!memcmp(actual,expected,(size_t)c.vocab*4));
        int32_t greedy=-1;ColiEdgeSelectRequest select={.struct_size=sizeof select,.rows=1,.input=a,.input_bytes=bytes,
            .token_ids=&greedy,.token_capacity=1};OK(coli_edge_select(edge,&select,error,sizeof error));
        assert(greedy==argmax(expected,c.vocab));ids[chat]=greedy;
    }
    for(int peer=0;peer<2;peer++) {
        for(int chat=0;chat<2;chat++)coli_segment_session_destroy(sessions[peer][chat]);
        ColiSegmentSessionOptions so={.struct_size=sizeof so,.context_tokens=80};
        OK(coli_segment_session_create(engines[peer],&so,&sessions[peer][0],error,sizeof error));
    }
    /* Reusing the resident engine starts a clean session, not yesterday's KV. */
    int32_t first=177;ColiEdgeEmbedRequest embed={.struct_size=sizeof embed,.rows=1,.token_ids=&first,.token_count=1,
        .output=a,.output_bytes=bytes};OK(coli_edge_embed(edge,&embed,error,sizeof error));
    for(int peer=0;peer<2;peer++) {
        ColiSegmentRunRequest run={.struct_size=sizeof run,.rows=1,.position=0,.token_ids=&first,.token_count=1,
            .input=a,.input_bytes=bytes,.output=b,.output_bytes=bytes};
        OK(coli_segment_run(sessions[peer][0],&run,error,sizeof error));float *swap=a;a=b;b=swap;
        coli_segment_session_destroy(sessions[peer][0]);OK(coli_segment_engine_close(engines[peer],error,sizeof error));
    }
    ColiEdgeLogitsRequest logits={.struct_size=sizeof logits,.rows=1,.input=a,.input_bytes=bytes,
        .logits=actual,.logits_capacity=(size_t)c.vocab};OK(coli_edge_logits(edge,&logits,error,sizeof error));
    lmb_v41_state_reset(&refs[0]);forward(&refs[0],&first,1,expected);assert(!memcmp(actual,expected,(size_t)c.vocab*4));
    assert(!forbidden_reads);
    for(int i=0;i<2;i++){free(feedback[i]);lmb_v41_model_destroy(&refs[i],0);}lmb_v41_model_destroy(&reference,1);
    coli_edge_engine_close(edge);free(a);free(b);free(expected);free(actual);
    printf("V41 public Edge/Segment ABI: cut %d, two isolated chats, 48 exact logits, clean reopen; bounded opens refused.\n",cut);
#undef OK
}
int main(int argc,char **argv) {
    assert(argc==2);setenv("CTX","128",1);setenv("PIN","off",1);unsetenv("V41_INDEX_OWNER");
    for(int cut=1;cut<=5;cut++) {
        fflush(NULL);pid_t pid=fork();assert(pid>=0);
        if(!pid){prove(argv[1],cut);return 0;}
        int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("V41 ABI LAB PASS. Not registered in shipped binaries; bounded checkpoint admission still required.");return 0;
}
