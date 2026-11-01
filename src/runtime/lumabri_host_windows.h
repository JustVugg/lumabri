/* One approved native Edge process and one shared production gateway per
 * allocation. The keeper owns every handle until the gateway and job exit. */
#ifndef LMB_HOST_WINDOWS_H
#define LMB_HOST_WINDOWS_H
#include "lumabri_home.h"
#include "lumabri_ready.h"
#include "lumabri_run_gate.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "src/runtime/lumabri_host_protocol.h"
#include "src/runtime/lumabri_host_sessions.h"

typedef struct {
    LmbOwnedProcess child;
    LmbHostEngine engine;
    LmbHomeOffer offer;
    LmbSocket listener;
    pthread_t thread;
    HANDLE ready,done;
    _Atomic int stop,sealed;
    int started;
    uint32_t port;
    char root[65];
} LmbWindowsHost;

static int lmb_windows_host_ready(void *opaque) {
    LmbWindowsHost *h=opaque;
    return SetEvent(h->ready) ? 0 : -1;
}
static void lmb_windows_host_sealed(void *opaque) {
    LmbWindowsHost *h=opaque;atomic_store(&h->sealed,1);
}

/* Readiness and capabilities come from the actual owned engine. Bound both
 * time and output, and consume the complete startup frame before sharing the
 * codec with clients. In particular, an open TCP listener is not READY. */
static int lmb_windows_host_capabilities(LmbWindowsHost *h) {
    uint64_t deadline=GetTickCount64()+600000;
    char line[1024];size_t used=0,total=0;unsigned fields=0;int ready=0;
    while (!host_engine_stopping(&h->engine) && GetTickCount64()<deadline) {
        if (WaitForSingleObject(h->child.process,0)!=WAIT_TIMEOUT) return -1;
        DWORD available=0;
        if (!PeekNamedPipe(h->engine.from,NULL,0,NULL,&available,NULL)) return -1;
        if (!available) {Sleep(10);continue;}
        char c;
        if (++total>65536 || lmb_pipe_io(h->engine.from,&c,1,0,deadline) || !c) return -1;
        if (c!='\n') {
            if (used+1>=sizeof line) return -1;
            line[used++]=c;continue;
        }
        line[used]=0;used=0;
        if (ready) return !strcmp(line,"STAT 0 0 0 0") && fields==31 ? 0 : -1;
        if (!strcmp(line,"\x01\x01" "READY" "\x01\x01")) {ready=1;continue;}
        unsigned bit=0;
        if (!strcmp(line,"LUMABRI_RESET 1")) {bit=1;h->engine.reset_supported=1;}
        else if (!strcmp(line,"LUMABRI_REQUEST_SEED 1")) {bit=2;h->engine.request_seed_supported=1;}
        else if (!strncmp(line,"LUMABRI_SLOTS ",14)) {
            bit=4;
            if (lmb_session_limit_parse(line+14,LMB_HOST_MAX_SESSIONS,&h->engine.session_slots) ||
                h->engine.session_slots!=(h->offer.sessions ? h->offer.sessions : 1)) return -1;
        } else if (!strncmp(line,"LUMABRI_NUMERIC ",16)) {
            bit=8;char *space=strchr(line+16,' ');
            if (!space) return -1;
            *space++=0;
            if (lmb_session_limit_parse(line+16,UINT32_MAX,&h->engine.numeric_abi) ||
                !*space || strlen(space)>=sizeof h->engine.numeric_class) return -1;
            for (const char *p=space;*p;p++) if ((unsigned char)*p<33 || (unsigned char)*p>126) return -1;
            strcpy(h->engine.numeric_class,space);
        } else if (!strncmp(line,"LUMABRI_SAMPLING ",17)) {
            bit=16;
            if (!strcmp(line+17,"GREEDY")) h->engine.greedy_only=1;
            else if (strcmp(line+17,"LOGITS")) return -1;
        } else if (!strcmp(line,"LUMABRI_VISION 1")) h->engine.vision_supported=1;
        else if (!strncmp(line,"LUMABRI_",8)) return -1;
        if (fields&bit) return -1;
        fields|=bit;
    }
    return -1;
}
static void *lmb_windows_host_thread(void *opaque) {
    LmbWindowsHost *h=opaque;
    if (!lmb_windows_host_capabilities(h)) {
        HostState state={.engine=&h->engine,.model_type=h->offer.model_type,.engine_kind="olmoe",
            .max_frame=1u<<20,.max_new=h->offer.max_new,.idle_seconds=300,.request_seconds=900,
            .client_key=h->offer.requester,.model_root=h->root,.slots=h->engine.session_slots,
            .ready=lmb_windows_host_ready,.sealed=lmb_windows_host_sealed,.lifecycle=h};
        (void)host_sessions_run(h->listener,&state,30000);
    } else fprintf(stderr,"[native-host] Edge startup did not confirm the approved capabilities.\n");
    SetEvent(h->done);return NULL;
}

/* Return pending while readers still own the pipes or descendants still hold
 * the job/lifetime handles. The donor must retain the entire reservation. */
static int lmb_windows_host_close(LmbWindowsHost *h) {
    atomic_store(&h->stop,1);
    if (h->started) {
        if (WaitForSingleObject(h->done,0)!=WAIT_OBJECT_0) return -1;
        if (pthread_join(h->thread,NULL)) return -1;
        h->started=0;
    }
    if (h->child.process && (lmb_owned_process_stop(&h->child,0,1000) ||
        lmb_owned_process_close(&h->child))) return -1;
    if (h->engine.to && h->engine.to!=INVALID_HANDLE_VALUE) CloseHandle(h->engine.to);
    if (h->engine.from && h->engine.from!=INVALID_HANDLE_VALUE) CloseHandle(h->engine.from);
    if (h->ready) CloseHandle(h->ready);
    if (h->done) CloseHandle(h->done);
    if (h->listener!=LMB_INVALID_SOCKET) lmb_socket_close(h->listener);
    memset(h,0,sizeof *h);h->listener=LMB_INVALID_SOCKET;return 0;
}

static int lmb_windows_host_start(LmbWindowsHost *h,const char *binary,const char *folder,
    const LmbHomeOffer *o,const char *compute,uint64_t reserve,HANDLE lifetime,HANDLE lease,HANDLE log) {
    if (h->started || h->child.process || !o->runs_edge) return -1;
    uint32_t slots=o->sessions ? o->sessions : 1;
    /* Codec buffers and gateway threads live in the keeper, not the Edge
     * process. Do not let both spend the same approved bytes. */
    uint64_t gateway_bytes=(uint64_t)slots*LMB_HOST_SESSION_BYTES;
    if (o->edge_ram_bytes<=gateway_bytes+(32u<<20)) return -1;
    memset(h,0,sizeof *h);h->listener=LMB_INVALID_SOCKET;h->offer=*o;
    h->engine.segment=1;h->engine.stop=&h->stop;
    lmb_hex(h->root,o->model_root,32);
    char signer[65],bytes[32],context[16],session_text[16],ram[100],reserve_env[100];
    char broker[1600],host_sessions[64],omp[64],omp_limit[64];
    lmb_hex(signer,o->requester,32);snprintf(bytes,sizeof bytes,"%llu",(unsigned long long)o->model_bytes);
    snprintf(context,sizeof context,"%u",o->context);snprintf(session_text,sizeof session_text,"%u",slots);
    snprintf(ram,sizeof ram,"LUMABRI_EDGE_RAM_BYTES=%llu",(unsigned long long)(o->edge_ram_bytes-gateway_bytes));
    snprintf(reserve_env,sizeof reserve_env,"LUMABRI_EDGE_RAM_RESERVE_MB=%llu",(unsigned long long)(reserve>>20));
    if (snprintf(broker,sizeof broker,"LUMABRI_COMPUTE_SOCKET=%s",compute)>=(int)sizeof broker) return -1;
    snprintf(host_sessions,sizeof host_sessions,"LUMABRI_HOST_SESSIONS=%u",slots);
    snprintf(omp,sizeof omp,"OMP_NUM_THREADS=%u",o->threads);snprintf(omp_limit,sizeof omp_limit,"OMP_THREAD_LIMIT=%u",o->threads);
    const char *args[]={"--engine","olmoe","--model-dir",folder,"--model",o->model,"--tracker",o->tracker,
        "--model-root",h->root,"--tokenizer-root",h->root,"--context",context,"--max-rows","16",
        "--sessions",session_text,"--serve","--direct-only","--checkpoint-signer",signer,"--model-bytes",bytes};
    const char *env[]={ram,reserve_env,broker,host_sessions,omp,omp_limit,
        "LUMABRI_RESIDENT_REQUIRED=1","LUMABRI_RESIDENCY_POLICY=engine-locked","LUMABRI_ENGINE_BACKEND=cpu",
        "LUMABRI_ENCRYPT=1","PIN=off","PILOT=0","HOT=0","COLI_NO_OMP_TUNE=1"};
    HANDLE input[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE},output[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
    h->ready=CreateEventW(NULL,TRUE,FALSE,NULL);h->done=CreateEventW(NULL,TRUE,FALSE,NULL);
    int port=0;h->listener=lmb_home_listen_service(&port);h->port=(uint32_t)port;
    int bad=!h->ready || !h->done || h->listener==LMB_INVALID_SOCKET ||
        lmb_pipe_process_pair(input) || lmb_pipe_process_pair(output);
    if (!bad) {
        h->engine.to=input[0];h->engine.from=output[0];input[0]=output[0]=INVALID_HANDLE_VALUE;
        LmbChildHandle handles[]={{"LUMABRI_ALLOCATION_LIFETIME_HANDLE",lifetime},{"LUMABRI_MACHINE_LEASE_HANDLE",lease}};
        LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=sizeof args/sizeof *args,
            .environment=env,.nenvironment=sizeof env/sizeof *env,.input=input[1],.output=output[1],.error=log,
            .handles=handles,.nhandles=sizeof handles/sizeof *handles};
        bad=lmb_owned_process_start(&h->child,&launch);
    }
    for (unsigned i=0;i<2;i++) {
        if (input[i]!=INVALID_HANDLE_VALUE) CloseHandle(input[i]);
        if (output[i]!=INVALID_HANDLE_VALUE) CloseHandle(output[i]);
    }
    if (!bad) {bad=pthread_create(&h->thread,NULL,lmb_windows_host_thread,h);if (!bad) h->started=1;}
    return bad ? -1 : 0; /* caller owns rollback, including failed startup */
}
#endif
