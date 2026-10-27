/* Optional native memory regression with an existing OLMoE checkpoint.
 * Exercises tokenizer, embedding and head; this is NOT a decode benchmark.
 * Does not raise limits, alter a checkpoint, or contact any peer. */
#define _GNU_SOURCE
#include "edge_adapters.h"
#include "edge_runtime.h"
#include "src/runtime/lumabri_engine_memory.h"
#include "src/runtime/lumabri_process_memory.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec+ts.tv_nsec/1e9;
}
static int report(const char *stage, uint64_t budget) {
    LmbEngineMemory owned;
    LmbProcessMemory process={0};
    lmb_engine_memory_snapshot(&owned);
    int rc=lmb_process_memory_probe(&process,1);
    struct rusage usage={0};
    int peak_known=!getrusage(RUSAGE_SELF,&usage);
    uint64_t peak=(uint64_t)usage.ru_maxrss;
#ifndef __APPLE__
    peak*=1024u;
#endif
    int valid=!rc && owned.enabled && !owned.fault && owned.allocations &&
        owned.locked_bytes>=owned.requested_bytes && lmb_process_memory_charge(&process)<=budget &&
        peak_known && peak<=budget;
    printf("{\"stage\":\"%s\",\"valid\":%s,\"budget_bytes\":%llu,\"allocations\":%llu,"
        "\"requested_bytes\":%llu,\"owned_locked_bytes\":%llu,\"peak_rss_bytes\":%llu,\"process\":",
        stage,valid?"true":"false",(unsigned long long)budget,(unsigned long long)owned.allocations,
        (unsigned long long)owned.requested_bytes,(unsigned long long)owned.locked_bytes,(unsigned long long)peak);
    lmb_process_memory_json(stdout,&process); puts("}"); fflush(stdout);
    return valid ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc<3 || argc>4 || (argc==4 && strcmp(argv[3],"--wait"))) {
        fprintf(stderr,"usage: %s CHECKPOINT APPROVED_EDGE_BYTES [--wait]\n",argv[0]); return 2;
    }
    char *end=NULL; errno=0;
    unsigned long long budget=strtoull(argv[2],&end,10);
    if (errno || !*argv[2] || *end || *argv[2]=='-' || !budget) return 2;
    if (argc==4) {
        printf("Memory probe PID %ld. Waiting for Enter; no model is loaded yet.\n",(long)getpid());
        fflush(stdout);
        if (getchar()==EOF) return 2;
    }
    if (setenv("LUMABRI_RESIDENCY_POLICY","engine-locked",1) ||
        coli_olmoe_edge_adapter_register()) return 1;
    ColiEdgeEngineOptions options={.struct_size=sizeof options,.model_dir=argv[1],
        .memory_limit_bytes=budget,.backend_mask=COLI_EDGE_CAP_CPU};
    ColiEdgeEngine *edge=NULL;
    char error[256]="";
    double started=now();
    if (coli_edge_engine_open("olmoe",&options,&edge,error,sizeof error)) {
        fprintf(stderr,"Edge open failed: %s\n",error); return 1;
    }
    printf("{\"edge_prepare_seconds\":%.6f}\n",now()-started);
    int rc=report("ready",budget);
    ColiEdgeCapabilities caps={.struct_size=sizeof caps};
    if (coli_edge_engine_capabilities(edge,&caps,error,sizeof error)) rc=-1;
    float *state=NULL;
    if (!rc) state=lmb_engine_calloc(caps.state_width,sizeof *state);
    if (!state) rc=-1;
    for (int i=0;!rc && i<32;i++) {
        const char *prompt="Describe a quiet morning by the sea.";
        int32_t tokens[128], selected=0;
        size_t count=0;
        if (coli_edge_tokenize(edge,prompt,strlen(prompt),tokens,128,&count,error,sizeof error) || !count) { rc=-1; break; }
        ColiEdgeEmbedRequest embed={.struct_size=sizeof embed,.rows=1,.token_ids=tokens,
            .token_count=1,.output=state,.output_bytes=(size_t)caps.state_width*sizeof *state};
        ColiEdgeSelectRequest select={.struct_size=sizeof select,.rows=1,.input=state,
            .input_bytes=embed.output_bytes,.token_ids=&selected,.token_capacity=1};
        if (coli_edge_embed(edge,&embed,error,sizeof error) || coli_edge_select(edge,&select,error,sizeof error)) rc=-1;
    }
    if (report("after_edge_operations",budget)) rc=-1;
    lmb_engine_free(state);
    coli_edge_engine_close(edge);
    if (rc) fprintf(stderr,"Edge memory regression failed: %s\n",error);
    return rc ? 1 : 0;
}
