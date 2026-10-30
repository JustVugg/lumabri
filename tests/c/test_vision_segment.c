#include "edge_adapters.h"
#include "edge_runtime.h"
#include "segment_adapters.h"
#include "segment_runtime.h"
#include "src/runtime/lumabri_vision.h"
#include "src/runtime/lumabri_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static void *read_file(const char *root,const char *name,size_t *bytes) {
    char path[4096]; assert(snprintf(path,sizeof path,"%s/%s",root,name)<(int)sizeof path);
    FILE *f=fopen(path,"rb"); assert(f); assert(!fseek(f,0,SEEK_END)); long n=ftell(f);
    assert(n>0 && n<8*1024*1024 && !fseek(f,0,SEEK_SET));
    void *p=malloc((size_t)n); assert(p && fread(p,1,(size_t)n,f)==(size_t)n); fclose(f); *bytes=(size_t)n; return p;
}
static void generate(const char *root,ColiEdgeEngine *edge,ColiEdgeCapabilities *cap,
                     const int32_t *tokens,size_t count,int ranges,int32_t out[4]) {
    ColiSegmentEngine *engines[2]={0}; ColiSegmentSession *sessions[2]={0}; char error[512]="";
    for (int i=0;i<ranges;i++) {
        ColiSegmentEngineOptions eo={.struct_size=sizeof eo,.model_dir=root,.context_tokens=2048,
            .layer_begin=(uint32_t)i*cap->num_layers/(uint32_t)ranges,
            .layer_end=(uint32_t)(i+1)*cap->num_layers/(uint32_t)ranges};
        int rc=coli_segment_engine_open("qwen38",&eo,&engines[i],error,sizeof error);
        if (rc) fprintf(stderr,"open: %s\n",error);
        assert(!rc);
        ColiSegmentSessionOptions so={.struct_size=sizeof so,.context_tokens=2048};
        assert(!coli_segment_session_create(engines[i],&so,&sessions[i],error,sizeof error));
    }
    size_t bytes=cap->state_width*sizeof(float);
    float *a=malloc(bytes),*b=malloc(bytes); assert(a && b);
    for (size_t pos=0;pos<count+3;pos++) {
        int32_t token=pos<count ? tokens[pos] : out[pos-count];
        ColiEdgeEmbedRequest er={.struct_size=sizeof er,.rows=1,.token_ids=&token,.token_count=1,.output=a,.output_bytes=bytes};
        assert(!coli_edge_embed(edge,&er,error,sizeof error));
        if (pos<count) assert(!lmb_vision_overlay(pos,1,a,cap->state_width));
        for (int i=0;i<ranges;i++) {
            ColiSegmentRunRequest rr={.struct_size=sizeof rr,.rows=1,.position=pos,.token_ids=&token,
                .token_count=1,.input=a,.input_bytes=bytes,.output=b,.output_bytes=bytes};
            int rc=coli_segment_run(sessions[i],&rr,error,sizeof error);
            if (rc) fprintf(stderr,"run: %s\n",error);
        assert(!rc);
            float *swap=a; a=b; b=swap;
        }
        if (pos+1>=count) {
            ColiEdgeSelectRequest sr={.struct_size=sizeof sr,.rows=1,.input=a,.input_bytes=bytes,
                .token_ids=&out[pos+1-count],.token_capacity=1};
            assert(!coli_edge_select(edge,&sr,error,sizeof error));
        }
    }
    free(a);free(b);
    for(int i=0;i<ranges;i++) { coli_segment_session_destroy(sessions[i]); assert(!coli_segment_engine_close(engines[i],error,sizeof error)); }
}
int main(int argc,char **argv) {
    assert(argc==2); char error[512]="";
    assert(!coli_qwen38_edge_adapter_register() && !coli_qwen38_segment_adapter_register());
    ColiEdgeEngine *edge=NULL; ColiEdgeEngineOptions eo={.struct_size=sizeof eo,.model_dir=argv[1]};
    int rc=coli_edge_engine_open("qwen38",&eo,&edge,error,sizeof error);
    if(rc) fprintf(stderr,"%s\n",error);
    assert(!rc && lmb_vision_available());
    ColiEdgeCapabilities cap={.struct_size=sizeof cap}; assert(!coli_edge_engine_capabilities(edge,&cap,error,sizeof error));
    double previous=0;
    for(unsigned image=0;image<2;image++) {
        char name[64]; snprintf(name,sizeof name,"image-%u.png",image); size_t n;
        unsigned char *png=read_file(argv[1],name,&n),*frame=calloc(1,n+33); assert(frame);
        memcpy(frame,"LMBIMG01",8); lmb_media_put(frame+8,1); lmb_media_put(frame+12,1); frame[16]='X';
        lmb_media_put(frame+17,0); lmb_media_put(frame+21,1); lmb_media_put(frame+25,1); lmb_media_put(frame+29,(uint32_t)n);
        memcpy(frame+33,png,n); char *prompt=NULL; size_t length=0;
        assert(!lmb_vision_prepare(frame,n+33,&prompt,&length)); free(png);free(frame);
        size_t count=0; assert(!coli_edge_tokenize(edge,prompt,length,NULL,0,&count,error,sizeof error));
        assert(count && count<2048); int32_t *tokens=calloc(count,sizeof *tokens); assert(tokens);
        assert(!coli_edge_tokenize(edge,prompt,length,tokens,count,&count,error,sizeof error));
        assert(!lmb_vision_tokens(tokens,count));
        float *rows=calloc(count*cap.state_width,sizeof(float)); assert(rows);
        assert(!lmb_vision_overlay(0,(uint32_t)count,rows,cap.state_width));
        snprintf(name,sizeof name,"image-%u.rows",image); size_t size;
        float *reference=read_file(argv[1],name,&size); unsigned images=0; double maximum=0,sum=0;
        for(size_t i=0;i<count;i++) if (tokens[i]==257) {
            assert((size_t)(images+1)*32*sizeof(float)<=size);
            for(unsigned c=0;c<cap.state_width;c++) {
                float expected=reference[images*32+c%32],actual=rows[i*cap.state_width+c];
                double error_abs=fabs((double)expected-actual); if(error_abs>maximum) maximum=error_abs;
                assert(isfinite(actual) && error_abs<.002*(1+fabs((double)expected))); sum+=actual;
            }
            images++;
        }
        assert(images*32*sizeof(float)==size);
        if(image) assert(fabs(sum-previous)>1e-3);
        previous=sum;
        int32_t full[4],split[4]; generate(argv[1],edge,&cap,tokens,count,1,full); generate(argv[1],edge,&cap,tokens,count,2,split);
        assert(!memcmp(full,split,sizeof full));
        printf("VISION: image %u, %u real image tokens, independent tower max error %.8g, local/split greedy parity PASS\n",image,images,maximum);
        free(reference); free(rows); free(tokens); free(prompt); lmb_vision_clear();
    }
    coli_edge_engine_close(edge); assert(!lmb_vision_available()); return 0;
}
