/* Actual Colibri Edge + Segment inference on native Windows. The checkpoint
 * is supplied by the caller; this does not change any machine-wide limit. */
#include "edge_runtime.h"
#include "edge_adapters.h"
#include "segment_runtime.h"
#include "segment_adapters.h"
#include "src/runtime/lumabri_engine_memory.h"
#include "src/runtime/lumabri_engine_io_windows.h"
#include "src/runtime/lumabri_process_memory.h"
#define LMB_ENGINE_MEMORY_LINKED
#include "src/runtime/lumabri_resident.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
static const uint64_t budget=128u*1024u*1024u;
static ColiSegmentEngine *open_range(const char *model,unsigned first,unsigned end) {
    ColiSegmentEngineOptions options={.struct_size=sizeof options,
        .model_dir=model,.layer_begin=first,.layer_end=end,.context_tokens=64,
        .memory_limit_bytes=budget,.backend_mask=COLI_SEGMENT_CAP_CPU};
    ColiSegmentEngine *engine=NULL;
    if(coli_segment_engine_open("olmoe",&options,&engine,error,sizeof error)) {
        fprintf(stderr,"open range %u:%u: %s\n",first,end,error); abort();
    }
    return engine;
}
static ColiSegmentSession *session(ColiSegmentEngine *engine) {
    ColiSegmentSessionOptions options={.struct_size=sizeof options,.context_tokens=64,
        .memory_limit_bytes=budget};
    ColiSegmentSession *result=NULL;
    assert(!coli_segment_session_create(engine,&options,&result,error,sizeof error));
    return result;
}
static void run(ColiSegmentSession *session,unsigned position,int32_t token,
                float *input,float *output,size_t size) {
    assert(!lmb_resident_memory_check());
    ColiSegmentRunRequest request={.struct_size=sizeof request,.rows=1,
        .position=position,.token_ids=&token,.token_count=1,.input=input,
        .input_bytes=size,.output=output,.output_bytes=size};
    if(coli_segment_run(session,&request,error,sizeof error)) {
        fprintf(stderr,"run: %s\n",error); abort();
    }
}
static int32_t select_token(ColiEdgeEngine *edge,float *state,size_t size) {
    int32_t token=-1;
    ColiEdgeSelectRequest request={.struct_size=sizeof request,.rows=1,
        .input=state,.input_bytes=size,.token_ids=&token,.token_capacity=1};
    assert(!coli_edge_select(edge,&request,error,sizeof error));
    return token;
}
static void compare(ColiEdgeEngine *edge,ColiSegmentEngine **engines,
                    unsigned width,unsigned seed) {
    ColiSegmentSession *sessions[3];
    for(unsigned i=0;i<3;i++) sessions[i]=session(engines[i]);
    size_t size=width*sizeof(float);
    float *input=lmb_engine_malloc(size), *full=lmb_engine_malloc(size);
    float *first=lmb_engine_malloc(size), *split=lmb_engine_malloc(size);
    assert(input && full && first && split);
    PSAPI_WORKING_SET_EX_INFORMATION page={0};page.VirtualAddress=input;
    assert(QueryWorkingSetEx(GetCurrentProcess(),&page,sizeof page));
    assert(page.VirtualAttributes.Valid && page.VirtualAttributes.Locked);
    int32_t token=(int32_t)seed;
    for(unsigned position=0;position<32;position++) {
        ColiEdgeEmbedRequest embed={.struct_size=sizeof embed,.rows=1,
            .token_ids=&token,.token_count=1,.output=input,.output_bytes=size};
        assert(!coli_edge_embed(edge,&embed,error,sizeof error));
        run(sessions[0],position,token,input,full,size);
        run(sessions[1],position,token,input,first,size);
        run(sessions[2],position,token,first,split,size);
        for(unsigned i=0;i<width;i++) {
            assert(isfinite(full[i]) && isfinite(split[i]));
            assert(fabsf(full[i]-split[i])<=2e-5f*fmaxf(1,fmaxf(fabsf(full[i]),fabsf(split[i]))));
        }
        int32_t expected=select_token(edge,full,size);
        assert(expected==select_token(edge,split,size));
        token=expected;
    }
    for(unsigned i=0;i<3;i++) coli_segment_session_destroy(sessions[i]);
    lmb_engine_free(input); lmb_engine_free(full);
    lmb_engine_free(first); lmb_engine_free(split);
}
int main(int argc,char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    assert(argc==2);
    /* This fixture's three ranges plus Edge have a bounded test allowance.
     * The allocator itself must never enlarge it. */
    assert(SetProcessWorkingSetSize(GetCurrentProcess(),64u*1024u*1024u,budget));
    assert(!_putenv_s("LUMABRI_RESIDENCY_POLICY","engine-locked"));
    assert(!_putenv_s("LUMABRI_RESIDENT_REQUIRED","1"));
    assert(!_putenv_s("PIN","off"));
    assert(!_putenv_s("PILOT","0"));
    assert(!_putenv_s("HOT","0"));
    assert(!_putenv_s("COLI_NO_OMP_TUNE","1"));
    assert(!coli_olmoe_edge_adapter_register() && !coli_olmoe_segment_adapter_register());
    assert(lmb_engine_input_seal()==-1); /* unprepared cannot become READY */
    ColiEdgeEngine *edge=NULL;
    ColiEdgeEngineOptions options={.struct_size=sizeof options,.model_dir=argv[1],
        .memory_limit_bytes=budget,.backend_mask=COLI_EDGE_CAP_CPU};
    if(coli_edge_engine_open("olmoe",&options,&edge,error,sizeof error)) {
        fprintf(stderr,"edge: %s\n",error); return 1;
    }
    ColiEdgeCapabilities caps={.struct_size=sizeof caps};
    assert(!coli_edge_engine_capabilities(edge,&caps,error,sizeof error));
    assert(caps.num_layers>=2 && caps.vocab_size>=3);
    ColiSegmentEngine *engines[]={open_range(argv[1],0,caps.num_layers),
        open_range(argv[1],0,caps.num_layers/2),
        open_range(argv[1],caps.num_layers/2,caps.num_layers)};
    for(unsigned i=0;i<3;i++) {
        ColiSegmentCapabilities range={.struct_size=sizeof range};
        assert(!coli_segment_engine_capabilities(engines[i],&range,error,sizeof error));
        assert(range.state_width==caps.state_width && !strcmp(range.numeric_class,caps.numeric_class));
    }
    LmbEngineMemory memory;
    lmb_engine_memory_snapshot(&memory);
    assert(memory.enabled && !memory.fault && memory.allocations && memory.locked_bytes);
    lmb_resident_process_guard.process_budget=budget;
    assert(!lmb_resident_seal());
    assert(lmb_resident_process_guard.engine_locked && !lmb_resident_memory_check());
    LmbEngineInput before,after;lmb_engine_input_snapshot(&before);
    assert(before.sealed && before.read_calls && before.read_bytes && !before.denied_reads);
    compare(edge,engines,caps.state_width,1);
    compare(edge,engines,caps.state_width,2); /* fresh isolated conversation */
    lmb_engine_input_snapshot(&after);
    assert(after.read_calls==before.read_calls && after.read_bytes==before.read_bytes && !after.denied_reads);
    LmbProcessMemory process;
    assert(!lmb_process_memory_probe(&process,1) && lmb_process_memory_charge(&process)<=budget);
    printf("{\"native\":\"windows\",\"tokens_compared\":64,\"locked_bytes\":%llu,\"inference_input_bytes\":0,\"process\":",
        (unsigned long long)memory.locked_bytes);
    lmb_process_memory_json(stdout,&process);puts("}");
    LmbResidentGuard over_budget={.process_budget=1};
    assert(lmb_resident_guard_begin(&over_budget)==-1);
    assert(atomic_load(&over_budget.fault)==LMB_RESIDENT_OVER_BUDGET);
    /* A post-seal request cannot accidentally reopen checkpoint input. */
    char byte;DWORD received=123;
    assert(!lmb_engine_ReadFile(INVALID_HANDLE_VALUE,&byte,1,&received,NULL));
    assert(!received && GetLastError()==ERROR_ACCESS_DENIED);
    errno=0;
    assert(!lmb_engine_fread(&byte,1,1,NULL) && errno==EACCES);
    assert(lmb_resident_memory_check()==-1);
    assert(atomic_load(&lmb_resident_process_guard.fault)==LMB_RESIDENT_LOCK_FAILED);
    assert(!_putenv_s("LUMABRI_RESIDENCY_POLICY","locked"));
    LmbResidentGuard unsupported={0};
    assert(lmb_resident_guard_begin(&unsupported)==-1 && !unsupported.locked);
    for(unsigned i=0;i<3;i++) assert(!coli_segment_engine_close(engines[i],error,sizeof error));
    coli_edge_engine_close(edge);
    lmb_engine_memory_snapshot(&memory);assert(!memory.fault);
    puts("WINDOWS SEGMENT: PASS (real Edge + full/split ranges, two conversations, resident input sealed)");
    return 0;
}
