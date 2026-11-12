/* Reuse the upstream ABI streaming helpers. Supply real Edge embeddings:
 * Mistral's boundary carries validated latent rows as well as hidden values. */
#define main upstream_state_test_main
#include "tests/test_segment_adapters_real.c"
#undef main
#include "edge_runtime.h"
#include "edge_adapters.h"
#include <assert.h>
#ifndef LMB_STATE_FAMILY
#define LMB_STATE_FAMILY "glm"
#endif
#define OK(x) do {if((x)){fprintf(stderr,"line %d: %s\n",__LINE__,error);abort();}}while(0)
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    char error[512]="state mismatch";OK(register_all());OK(coli_glm_edge_adapter_register());OK(coli_glm53_edge_adapter_register());OK(coli_kimi_edge_adapter_register());OK(coli_inkling_edge_adapter_register());OK(coli_qwen38_edge_adapter_register());
    ColiEdgeEngine *edge=NULL;ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=argv[1],.memory_limit_bytes=1ull<<30};
    OK(coli_edge_engine_open(LMB_STATE_FAMILY,&eo,&edge,error,sizeof error));
    ColiEdgeCapabilities ec={.struct_size=sizeof ec};OK(coli_edge_engine_capabilities(edge,&ec,error,sizeof error));
    uint32_t width=ec.state_width;size_t bytes=(size_t)width*40*sizeof(float);
    float *input=malloc(bytes),*full=malloc(bytes),*left=malloc(bytes),*chain=malloc(bytes),*other=malloc(bytes);
    assert(input && full && left && chain && other);
    int32_t tokens[40];for(int i=0;i<40;i++)tokens[i]=1+i%17;
    ColiEdgeEmbedRequest er={.struct_size=sizeof er,.rows=40,.token_ids=tokens,.token_count=40,.output=input,.output_bytes=bytes};
    OK(coli_edge_embed(edge,&er,error,sizeof error));
    for(uint32_t cut=1;cut<ec.num_layers;cut++) {
        ColiSegmentEngine *eng[3]={0};ColiSegmentCapabilities caps[3];
        OK(open_engine_limited(LMB_STATE_FAMILY,argv[1],0,ec.num_layers,64,1ull<<30,&eng[0],&caps[0],error,sizeof error));
        OK(open_engine_limited(LMB_STATE_FAMILY,argv[1],0,cut,64,1ull<<30,&eng[1],&caps[1],error,sizeof error));
        OK(open_engine_limited(LMB_STATE_FAMILY,argv[1],cut,ec.num_layers,64,1ull<<30,&eng[2],&caps[2],error,sizeof error));
        ColiSegmentSession *s[6]={0};
        for(int i=0;i<6;i++)OK(create_session(eng[i%3],64,&s[i],error,sizeof error));
        OK(run_rows(s[0],0,20,width,input,full,tokens,error,sizeof error));
        OK(run_rows(s[1],0,20,width,input,left,tokens,error,sizeof error));
        /* A second conversation runs between the two halves of the first. */
        OK(run_rows(s[4],0,13,width,input+3*width,other,tokens+3,error,sizeof error));
        OK(run_rows(s[5],0,13,width,other,chain,tokens+3,error,sizeof error));
        OK(run_rows(s[2],0,20,width,left,chain,tokens,error,sizeof error));
        OK(close_enough(full,chain,20*width,"chain with interleaved session"));
        for(int i=0;i<3;i++) {
            MemoryStream snap={0};OK(coli_segment_snapshot(s[i],memory_write,&snap,error,sizeof error));
            OK(coli_segment_restore(s[i+3],memory_read,&snap,error,sizeof error));free(snap.data);
        }
        OK(run_rows(s[0],20,20,width,input+20*width,full,tokens+20,error,sizeof error));
        OK(run_rows(s[3],20,20,width,input+20*width,other,tokens+20,error,sizeof error));
        OK(close_enough(full,other,20*width,"full restored continuation"));
        OK(run_rows(s[4],20,20,width,input+20*width,left,tokens+20,error,sizeof error));
        /* Malformed boundary must not advance the restored right session. */
        MemoryStream before={0},after={0};OK(coli_segment_snapshot(s[5],memory_write,&before,error,sizeof error));
        if(!strcmp(LMB_STATE_FAMILY,"glm")) {
            float saved=left[width-2];left[width-2]=NAN;
            assert(run_rows(s[5],20,20,width,left,chain,tokens+20,error,sizeof error));left[width-2]=saved;
        } else {
            assert(run_rows(s[5],21,20,width,left,chain,tokens+20,error,sizeof error));
        }
        OK(coli_segment_snapshot(s[5],memory_write,&after,error,sizeof error));
        assert(before.size==after.size && !memcmp(before.data,after.data,before.size));free(before.data);free(after.data);
        OK(run_rows(s[5],20,20,width,left,chain,tokens+20,error,sizeof error));
        OK(close_enough(full,chain,20*width,"restored chain continuation"));
        for(int i=0;i<6;i++)coli_segment_session_destroy(s[i]);
        for(int i=0;i<3;i++)OK(coli_segment_engine_close(eng[i],error,sizeof error));
    }
    coli_edge_engine_close(edge);free(input);free(full);free(left);free(chain);free(other);
    puts(LMB_STATE_FAMILY " STATE: PASS (all cuts, interleaved sessions, snapshot/restore, invalid request atomicity)");return 0;
}
