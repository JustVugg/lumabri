/* Independent GGUF models share a process, never metadata or conversation state. */
#define LMB_V41_ABI_NO_MAIN
#include "test_v41_abi.c"

typedef struct {
    ColiEdgeEngine *edge;
    ColiSegmentEngine *engine;
    ColiSegmentSession *session;
    ColiEdgeCapabilities cap;
    float *input,*output,*feedback,*logits,*reference;
    size_t bytes,aux,tail;
    int32_t token;
} Mixed;

static void mixed_close(Mixed *m) {
    coli_segment_session_destroy(m->session);m->session=NULL;
    assert(!coli_segment_engine_close(m->engine,NULL,0));m->engine=NULL;
    coli_edge_engine_close(m->edge);m->edge=NULL;
    free(m->input);free(m->output);free(m->feedback);free(m->logits);free(m->reference);
    memset(m,0,sizeof *m);
}
static void mixed_open(Mixed *m,const char *path) {
    char error[512]={0},name[2048];Cfg cfg;cfg_load(&cfg,path);
#define OK(x) do{if((x)){fprintf(stderr,"mixed ABI at %d: %s\n",__LINE__,error);abort();}}while(0)
    ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=path,.memory_limit_bytes=1ull<<30};
    OK(coli_edge_engine_open("deepseek_v41",&eo,&m->edge,error,sizeof error));
    m->cap.struct_size=sizeof m->cap;OK(coli_edge_engine_capabilities(m->edge,&m->cap,error,sizeof error));
    ColiSegmentEngineOptions so={.struct_size=sizeof so,.model_dir=path,.layer_end=m->cap.num_layers,
        .context_tokens=96,.memory_limit_bytes=1ull<<30};
    OK(coli_segment_engine_open("deepseek_v41",&so,&m->engine,error,sizeof error));
    ColiSegmentSessionOptions ss={.struct_size=sizeof ss,.context_tokens=80};
    OK(coli_segment_session_create(m->engine,&ss,&m->session,error,sizeof error));
    m->bytes=(size_t)m->cap.state_width*sizeof(float);m->aux=lmb_v41_state_aux(&cfg);
    m->tail=m->bytes-m->aux*sizeof(float);m->token=177;
    m->input=malloc(m->bytes);m->output=malloc(m->bytes);m->feedback=calloc(1,m->tail);
    m->logits=malloc((size_t)m->cap.vocab_size*sizeof(float));
    size_t cells=(size_t)m->cap.vocab_size*48*2;
    m->reference=malloc(cells*sizeof(float));
    assert(m->input&&m->output&&m->feedback&&m->logits&&m->reference);
    assert(snprintf(name,sizeof name,"%s/reference.bin",path)<(int)sizeof name);
    FILE *f=fopen(name,"rb");assert(f);
    assert(fread(m->reference,sizeof(float),cells,f)==cells && fgetc(f)==EOF && !ferror(f));fclose(f);
#undef OK
}
static void mixed_step(Mixed *m,int step) {
    char error[512]={0};
#define OK(x) do{if((x)){fprintf(stderr,"mixed RUN at %d: %s\n",__LINE__,error);abort();}}while(0)
    ColiEdgeEmbedRequest embed={.struct_size=sizeof embed,.rows=1,.token_ids=&m->token,.token_count=1,
        .output=m->input,.output_bytes=m->bytes};
    OK(coli_edge_embed(m->edge,&embed,error,sizeof error));
    if(step)memcpy(m->input+m->aux,m->feedback,m->tail);
    ColiSegmentRunRequest run={.struct_size=sizeof run,.rows=1,.position=(uint64_t)step,
        .token_ids=&m->token,.token_count=1,.input=m->input,.input_bytes=m->bytes,
        .output=m->output,.output_bytes=m->bytes};
    OK(coli_segment_run(m->session,&run,error,sizeof error));
    memcpy(m->feedback,m->output+m->aux,m->tail);
    ColiEdgeLogitsRequest head={.struct_size=sizeof head,.rows=1,.input=m->output,.input_bytes=m->bytes,
        .logits=m->logits,.logits_capacity=m->cap.vocab_size};
    OK(coli_edge_logits(m->edge,&head,error,sizeof error));
    assert(!memcmp(m->logits,m->reference+(size_t)step*2*m->cap.vocab_size,(size_t)m->cap.vocab_size*sizeof(float)));
    m->token=argmax(m->logits,(int)m->cap.vocab_size);
#undef OK
}
int main(int argc,char **argv) {
    assert(argc==4);assert(!lmb_v41_register_impl());setenv("CTX","128",1);setenv("PIN","off",1);
    for(int order=0;order<2;order++) {
        Mixed m[3]={{0}};
        for(int j=0;j<3;j++){int i=order?2-j:j;mixed_open(&m[i],argv[1+i]);}
        weights_sealed=1;
        for(int step=0;step<48;step++)for(int i=0;i<3;i++) {
            if(i==1 && step==16)mixed_close(&m[i]);
            if(m[i].engine)mixed_step(&m[i],step);
        }
        for(int i=0;i<3;i++)if(m[i].engine)mixed_close(&m[i]);
        assert(!forbidden_reads);weights_sealed=0;
    }
    puts("V41 MIXED GGUF: PASS (three formats, reversed open order, early unload, exact independent logits, no weight reads)");
    return 0;
}
