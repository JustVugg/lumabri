/* Native processes/threads exercise the same private donor compute contract
 * as Unix: bounded FIFO, cancellation, owner crash and no silent fallback. */
#define LMB_COMPUTE_BROKER_SERVER
#include "src/runtime/lumabri_compute_broker.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_ready.h"
#include <assert.h>

static void observed(_Atomic uint32_t *value,uint32_t wanted) {
    uint64_t until=lmb_compute_now()+5000;
    while (atomic_load(value)!=wanted && lmb_compute_now()<until) Sleep(1);
    assert(atomic_load(value)==wanted);
}
static _Atomic unsigned running,position;
static unsigned order[4];
typedef struct { unsigned id; uint32_t timeout; _Atomic int stop; int result; } Work;
static int cancelled(void *arg) { return atomic_load(&((Work *)arg)->stop); }
static void *work(void *arg) {
    Work *w=arg; LmbComputePermit permit;
    w->result=lmb_compute_acquire(&permit,w->timeout,cancelled,w);
    if (!w->result) {
        assert(!atomic_fetch_add(&running,1));
        unsigned n=atomic_fetch_add(&position,1); assert(n<4); order[n]=w->id;
        Sleep(10); assert(atomic_fetch_sub(&running,1)==1);
        lmb_compute_release(&permit);
    }
    return NULL;
}
static HANDLE connect_raw(const char *name,int anonymous) {
    uint64_t until=lmb_compute_now()+5000; HANDLE pipe;
    while ((pipe=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|
            (anonymous ? SECURITY_ANONYMOUS : SECURITY_IDENTIFICATION),NULL))==INVALID_HANDLE_VALUE) {
        assert(GetLastError()==ERROR_PIPE_BUSY && lmb_compute_now()<until); Sleep(5);
    }
    return pipe;
}
static void closed(HANDLE pipe) {
    uint8_t byte;
    assert(lmb_pipe_io(pipe,&byte,1,0,lmb_compute_now()+4000));
    CloseHandle(pipe);
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (argc==2 && !wcscmp(argv[1],L"--hold")) {
        LmbComputePermit permit;
        if (lmb_compute_acquire(&permit,5000,NULL,NULL) || lmb_ready_notify()) return 1;
        Sleep(INFINITE); return 2;
    }
    assert(argc==1);
    wchar_t temp[1024],directory[1200],exe[2048]; char binary[6144],path[1800];
    assert(GetTempPathW(1024,temp));
    assert(swprintf(directory,1200,L"%lslumabri-compute-%lu-%llu-\x00e8",temp,
        GetCurrentProcessId(),GetTickCount64())>0 && CreateDirectoryW(directory,NULL));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA",directory));
    assert(!lmb_private_default_path(path,sizeof path,"compute.sock"));
    wchar_t *compute_path=lmb_process_wide(path); assert(compute_path);
    assert(SetEnvironmentVariableW(L"LUMABRI_COMPUTE_SOCKET",compute_path)); free(compute_path);
    LmbComputeBroker broker,other;
    assert(!lmb_compute_broker_start(&broker,path));
    assert(lmb_compute_broker_start(&other,path));
    LmbComputePermit held;
    assert(!lmb_compute_acquire(&held,5000,NULL,NULL)); observed(&broker.active,1);
    pthread_t threads[3]; Work jobs[3]={{.id=1,.timeout=5000},{.id=2,.timeout=5000},{.id=3,.timeout=5000}};
    for (unsigned i=0;i<3;i++) {
        assert(!pthread_create(&threads[i],NULL,work,&jobs[i])); observed(&broker.queued,i+1);
    }
    lmb_compute_release(&held);
    for (unsigned i=0;i<3;i++) {
        pthread_join(threads[i],NULL); assert(!jobs[i].result && order[i]==i+1);
    }
    observed(&broker.active,0); observed(&broker.queued,0);
    assert(!lmb_compute_acquire(&held,5000,NULL,NULL));
    Work timeout={.timeout=100},cancel={.timeout=5000};
    assert(!pthread_create(&threads[0],NULL,work,&timeout));
    pthread_join(threads[0],NULL); assert(timeout.result==-1); observed(&broker.queued,0);
    assert(!pthread_create(&threads[0],NULL,work,&cancel)); observed(&broker.queued,1);
    atomic_store(&cancel.stop,1); pthread_join(threads[0],NULL);
    assert(cancel.result==-1); observed(&broker.queued,0);
    lmb_compute_release(&held); observed(&broker.active,0);

    assert(GetModuleFileNameW(NULL,exe,2048));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,exe,-1,binary,sizeof binary,NULL,NULL));
    HANDLE ready=CreateEventW(NULL,TRUE,FALSE,NULL); assert(ready);
    LmbChildHandle handoff={"LUMABRI_READY_HANDLE",ready};
    const char *args[]={"--hold"}; char env[1900];
    assert(snprintf(env,sizeof env,"LUMABRI_COMPUTE_SOCKET=%s",path)<(int)sizeof env);
    const char *environment[]={env};
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=1,.handles=&handoff,.nhandles=1,
        .environment=environment,.nenvironment=1,
        .error=GetStdHandle(STD_ERROR_HANDLE)};
    LmbOwnedProcess child={0}; assert(!lmb_owned_process_start(&child,&launch));
    assert(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0); observed(&broker.active,1);
    assert(!lmb_owned_process_stop(&child,0,5000) && !lmb_owned_process_close(&child));
    CloseHandle(ready);
    assert(!lmb_compute_acquire(&held,5000,NULL,NULL));
    lmb_compute_release(&held); observed(&broker.active,0);

    uint8_t request[8]={'L','C','P',1,0x88,0x13,0,0};
    HANDLE raw=connect_raw(broker.path,1);
    assert(!lmb_pipe_io(raw,request,8,1,lmb_compute_now()+1000)); closed(raw);
    raw=connect_raw(broker.path,0); request[0]='!';
    assert(!lmb_pipe_io(raw,request,8,1,lmb_compute_now()+1000)); closed(raw); request[0]='L';
    raw=connect_raw(broker.path,0);
    assert(!lmb_pipe_io(raw,request,1,1,lmb_compute_now()+1000)); closed(raw);
    assert(!lmb_compute_acquire(&held,5000,NULL,NULL)); observed(&broker.active,1);
    HANDLE queued[LMB_COMPUTE_PENDING_MAX-1];
    for (unsigned i=0;i<LMB_COMPUTE_PENDING_MAX-1;i++) {
        queued[i]=connect_raw(broker.path,0);
        assert(!lmb_pipe_io(queued[i],request,8,1,lmb_compute_now()+1000));
        observed(&broker.queued,i+1);
    }
    LmbComputePermit denied;
    assert(lmb_compute_acquire(&denied,500,NULL,NULL) && denied==LMB_COMPUTE_INVALID);
    for (unsigned i=0;i<LMB_COMPUTE_PENDING_MAX-1;i++) CloseHandle(queued[i]);
    observed(&broker.queued,0); lmb_compute_release(&held); observed(&broker.active,0);
    assert(!lmb_compute_acquire(&held,5000,NULL,NULL));
    lmb_compute_broker_stop(&broker); lmb_compute_release(&held);
    assert(lmb_compute_acquire(&held,100,NULL,NULL) && held==LMB_COMPUTE_INVALID);

    DWORD before,after; assert(GetProcessHandleCount(GetCurrentProcess(),&before));
    for (unsigned i=0;i<5;i++) {
        assert(!lmb_compute_broker_start(&broker,path));
        assert(!lmb_compute_acquire(&held,5000,NULL,NULL)); lmb_compute_release(&held);
        lmb_compute_broker_stop(&broker);
    }
    assert(GetProcessHandleCount(GetCurrentProcess(),&after) && after==before);
    assert(SetEnvironmentVariableW(L"LUMABRI_COMPUTE_SOCKET",NULL));
    assert(!lmb_compute_acquire(&held,100,NULL,NULL) && held==LMB_COMPUTE_INVALID);
    wchar_t folder[1250];
    assert(swprintf(folder,1250,L"%ls\\Lumabri",directory)>0);
    assert(RemoveDirectoryW(folder) && RemoveDirectoryW(directory));
    puts("NATIVE COMPUTE: PASS (private FIFO, overload, timeout/cancel, malformed/anonymous refusal, process crash, restart, no handle leaks)");
    return 0;
}
