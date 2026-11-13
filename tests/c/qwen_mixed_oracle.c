/* Alternating native and converted Segment engines must not reuse another
 * checkpoint's GGUF context, mode flags or expert-slot layout. Edge retains
 * its explicit one-tokenizer-per-process contract; prepare inputs in turn. */
#define main single_checkpoint_main
#include "edge_segment_greedy.c"
#undef main

typedef struct {
    ColiSegmentEngine *engine;
    ColiSegmentSession *session;
    int32_t full[512];
    uint32_t width,layers,nf;
    float *input,*output,*expected;
} MixedModel;

static void mixed_close(MixedModel *m) {
    coli_segment_session_destroy(m->session);m->session=NULL;
    if(m->engine)(void)coli_segment_engine_close(m->engine,NULL,0);
    m->engine=NULL;
}
static int mixed_open(MixedModel *m,const char *path) {
    char error[512]="mixed oracle setup failed";int rc=1;
    ColiSegmentEngineOptions options={.struct_size=sizeof options,.model_dir=path,
        .layer_end=m->layers,.context_tokens=m->nf,.memory_limit_bytes=1ull<<30};
    REQUIRE(!coli_segment_engine_open("qwen36",&options,&m->engine,error,sizeof error));
    ColiSegmentSessionOptions session={.struct_size=sizeof session,.context_tokens=m->nf};
    REQUIRE(!coli_segment_session_create(m->engine,&session,&m->session,error,sizeof error));rc=0;
done:
    if(rc)mixed_close(m);
    return rc;
}
static int mixed_step(MixedModel *m,uint32_t step,float *output) {
    char error[512]="mixed oracle RUN failed";size_t row=m->width*sizeof(float);
    ColiSegmentRunRequest run={.struct_size=sizeof run,.rows=1,.position=step,
        .token_ids=&m->full[step],.token_count=1,.input=m->input+(size_t)step*m->width,
        .input_bytes=row,.output=output,.output_bytes=row};
    int rc=coli_segment_run(m->session,&run,error,sizeof error);
    if(rc)fprintf(stderr,"%s\n",error);
    return rc;
}
static int mixed_reference(MixedModel *m,const char *path,const char *ref) {
    char error[512]="mixed oracle setup failed",text[65536];int rc=1;
    jval *json=NULL;ColiEdgeEngine *edge=NULL;
    FILE *f=fopen(ref,"rb");if(!f)return 1;
    size_t n=fread(text,1,sizeof text-1,f);int bad=ferror(f)||!feof(f);fclose(f);text[n]=0;
    REQUIRE(!bad && !memchr(text,0,n));json=json_parse_checked(text);REQUIRE(json);
    int nf=ids(json,"full_ids",m->full);REQUIRE(nf>0 && nf<=128);m->nf=(uint32_t)nf;
    ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=path,.memory_limit_bytes=1ull<<30};
    REQUIRE(!coli_edge_engine_open("qwen36",&eo,&edge,error,sizeof error));
    ColiEdgeCapabilities caps={.struct_size=sizeof caps};
    REQUIRE(!coli_edge_engine_capabilities(edge,&caps,error,sizeof error));
    m->width=caps.state_width;m->layers=caps.num_layers;
    size_t row=m->width*sizeof(float),bytes=m->nf*row;
    m->input=malloc(bytes);m->expected=malloc(bytes);m->output=malloc(row);
    REQUIRE(m->input && m->expected && m->output);
    ColiEdgeEmbedRequest er={.struct_size=sizeof er,.rows=m->nf,.token_ids=m->full,
        .token_count=m->nf,.output=m->input,.output_bytes=bytes};
    REQUIRE(!coli_edge_embed(edge,&er,error,sizeof error));
    coli_edge_engine_close(edge);edge=NULL;
    REQUIRE(!mixed_open(m,path));
    for(uint32_t i=0;i<m->nf;i++)REQUIRE(!mixed_step(m,i,m->expected+(size_t)i*m->width));
    rc=0;
done:
    mixed_close(m);coli_edge_engine_close(edge);json_free(json);return rc;
}
int main(int argc,char **argv) {
    if(argc!=7){fprintf(stderr,"usage: %s MODEL REF MODEL REF MODEL REF\n",argv[0]);return 2;}
    if(coli_qwen36_segment_adapter_register() || coli_qwen36_edge_adapter_register())return 2;
    MixedModel models[3]={{0}};int rc=1;
    for(int i=0;i<3;i++)if(mixed_reference(&models[i],argv[1+2*i],argv[2+2*i]))goto done;
    for(int order=0;order<2;order++) {
        for(int j=0;j<3;j++) {
            int i=order?2-j:j;if(mixed_open(&models[i],argv[1+2*i]))goto done;
        }
#ifdef LMB_ORACLE_GUARD_READS
        weights_sealed=1;
#endif
        for(uint32_t step=0;step<512;step++) {
            int active=0;
            for(int i=0;i<3;i++) {
                MixedModel *m=&models[i];if(!m->engine)continue;
                if(step>=m->nf || (i==1 && step==16)) {
                    mixed_close(m);continue; /* remaining engines keep running */
                }
                active=1;if(mixed_step(m,step,m->output))goto done;
                if(memcmp(m->output,m->expected+(size_t)step*m->width,m->width*sizeof(float))) {
                    fprintf(stderr,"mixed model %d output differs at step %u\n",i,step);goto done;
                }
            }
            if(!active)break;
        }
#ifdef LMB_ORACLE_GUARD_READS
        weights_sealed=0;if(forbidden_reads)goto done;
#endif
    }
    puts("QWEN MIXED: PASS (native/converted, alternating engines, early close, no inference reads)");rc=0;
done:
#ifdef LMB_ORACLE_GUARD_READS
    weights_sealed=0;
#endif
    for(int i=0;i<3;i++) {
        mixed_close(&models[i]);free(models[i].input);free(models[i].output);free(models[i].expected);
    }
    return rc;
}
