/* Independent fixed-token oracle through the public Edge/Segment ABI.
 * Greedy selection does not require the optional LOGITS capability. Run the
 * same reference with one range, two ranges, and fresh session state again. */
#include "edge_adapters.h"
#include "edge_runtime.h"
#include "segment_adapters.h"
#include "segment_runtime.h"
#include "json.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if(!(x)) { fprintf(stderr,"oracle line %d: %s\n",__LINE__,error); goto done; } } while(0)

static int ids(jval *root, const char *key, int32_t *out) {
    jval *a=json_get(root,key);
    if(!a || a->t!=J_ARR || a->len<1 || a->len>512) return -1;
    for(int i=0;i<a->len;i++) {
        jval *n=a->kids[i];
        if(!n || n->t!=J_NUM || n->num<0 || n->num>INT32_MAX || n->num!=(int32_t)n->num) return -1;
        out[i]=(int32_t)n->num;
    }
    return a->len;
}

static int run_oracle(const char *family, const char *path, ColiEdgeEngine *edge,
                      const ColiEdgeCapabilities *ec, int ranges,
                      const int32_t *prompt, int np, const int32_t *full, int nf) {
    char error[512]="contract mismatch";
    ColiSegmentEngine *engines[2]={0}; ColiSegmentSession *sessions[2]={0};
    float *a=NULL,*b=NULL; int rc=1;
    size_t row=(size_t)ec->state_width*sizeof(float);
    a=malloc((size_t)np*row); b=malloc((size_t)np*row);
    REQUIRE(a && b && ec->state_dtype==COLI_EDGE_DTYPE_F32 && ec->num_layers>=2);
    for(int i=0;i<ranges;i++) {
        ColiSegmentEngineOptions eo={.struct_size=sizeof(eo),.model_dir=path,
            .layer_begin=(uint32_t)i*ec->num_layers/(uint32_t)ranges,
            .layer_end=(uint32_t)(i+1)*ec->num_layers/(uint32_t)ranges,.context_tokens=(uint32_t)nf};
        REQUIRE(!coli_segment_engine_open(family,&eo,&engines[i],error,sizeof error));
        ColiSegmentCapabilities sc={.struct_size=sizeof(sc)};
        REQUIRE(!coli_segment_engine_capabilities(engines[i],&sc,error,sizeof error));
        REQUIRE(sc.state_width==ec->state_width && sc.state_dtype==ec->state_dtype &&
                !strcmp(sc.state_schema,ec->state_schema) && !strcmp(sc.numeric_class,ec->numeric_class));
        ColiSegmentSessionOptions so={.struct_size=sizeof(so),.context_tokens=(uint32_t)nf};
        REQUIRE(!coli_segment_session_create(engines[i],&so,&sessions[i],error,sizeof error));
    }
    for(int token=np;token<nf;token++) {
        uint32_t rows=token==np ? (uint32_t)np : 1;
        const int32_t *input=token==np ? prompt : &full[token-1];
        ColiEdgeEmbedRequest er={.struct_size=sizeof(er),.rows=rows,.token_ids=input,
            .token_count=rows,.output=a,.output_bytes=rows*row};
        REQUIRE(!coli_edge_embed(edge,&er,error,sizeof error));
        for(int i=0;i<ranges;i++) {
            ColiSegmentRunRequest rr={.struct_size=sizeof(rr),.rows=rows,
                .position=token==np ? 0 : (uint64_t)token-1,.token_ids=input,.token_count=rows,
                .input=a,.input_bytes=rows*row,.output=b,.output_bytes=rows*row};
            REQUIRE(!coli_segment_run(sessions[i],&rr,error,sizeof error));
            float *swap=a; a=b; b=swap;
        }
        int32_t next=-1;
        ColiEdgeSelectRequest sr={.struct_size=sizeof(sr),.rows=1,
            .input=a+(rows-1u)*ec->state_width,.input_bytes=row,.token_ids=&next,.token_capacity=1};
        REQUIRE(!coli_edge_select(edge,&sr,error,sizeof error));
        if(next!=full[token]) {
            snprintf(error,sizeof error,"token %d: got %d, oracle %d (%d ranges)",token,next,full[token],ranges);
            REQUIRE(0);
        }
    }
    printf("%s: %d ranges, %d independent oracle tokens PASS\n",family,ranges,nf-np);
    rc=0;
done:
    for(int i=0;i<2;i++) {
        coli_segment_session_destroy(sessions[i]);
        if(engines[i]) (void)coli_segment_engine_close(engines[i],NULL,0);
    }
    free(a);free(b);return rc;
}

int main(int argc, char **argv) {
    if(argc!=4) {fprintf(stderr,"usage: %s FAMILY CHECKPOINT ORACLE.json\n",argv[0]);return 2;}
    char error[512]="invalid oracle"; int rc=1;
    FILE *file=fopen(argv[3],"rb"); if(!file) return 2;
    char *text=malloc(1u<<20),*arena=NULL; jval *root=NULL;
    ColiEdgeEngine *edge=NULL;
    if(!text) {fclose(file);return 2;}
    size_t n=fread(text,1,(1u<<20)-1,file);
    int bad=ferror(file)||!feof(file); fclose(file); text[n]=0;
    REQUIRE(!bad);
    root=json_parse(text,&arena); REQUIRE(root);
    int32_t prompt[512],full[512];
    int np=ids(root,"prompt_ids",prompt),nf=ids(root,"full_ids",full);
    REQUIRE(np>0 && nf>np && !memcmp(prompt,full,(size_t)np*sizeof(*prompt)));
    REQUIRE(!coli_qwen38_segment_adapter_register() && !coli_qwen38_edge_adapter_register());
    REQUIRE(!coli_deepseek_v4_segment_adapter_register() && !coli_deepseek_v4_edge_adapter_register());
    ColiEdgeEngineOptions eo={.struct_size=sizeof(eo),.model_dir=argv[2]};
    REQUIRE(!coli_edge_engine_open(argv[1],&eo,&edge,error,sizeof error));
    ColiEdgeCapabilities ec={.struct_size=sizeof(ec)};
    REQUIRE(!coli_edge_engine_capabilities(edge,&ec,error,sizeof error));
    REQUIRE(ec.flags&COLI_EDGE_CAP_GREEDY);
    for(int i=0;i<nf;i++) REQUIRE((uint32_t)full[i]<ec.vocab_size);
    REQUIRE(!run_oracle(argv[1],argv[2],edge,&ec,1,prompt,np,full,nf));
    REQUIRE(!run_oracle(argv[1],argv[2],edge,&ec,2,prompt,np,full,nf));
    REQUIRE(!run_oracle(argv[1],argv[2],edge,&ec,2,prompt,np,full,nf));
    rc=0;
done:
    coli_edge_engine_close(edge);json_free(root);free(arena);free(text);return rc;
}
