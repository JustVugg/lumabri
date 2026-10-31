#include "lumabri_engine_io_windows.h"
#include "lumabri_engine_memory.h"
#include "lumabri_process_memory.h"
#include <errno.h>
#include <io.h>

static SRWLOCK input_lock=SRWLOCK_INIT;
static volatile LONG64 input_calls,input_bytes,input_denied;
static int input_sealed;
static volatile LONG input_failed;
static volatile LONG prepared;
static void *input_context;
static LmbEngineInputRead input_read;
static LmbEngineInputClose input_close;

int lmb_engine_input_provider(void *context,LmbEngineInputRead read,LmbEngineInputClose close) {
    AcquireSRWLockExclusive(&input_lock);
    int bad=!context || !read || !close || input_read || input_calls || input_sealed;
    if (!bad) {input_context=context;input_read=read;input_close=close;}
    ReleaseSRWLockExclusive(&input_lock);
    if (bad) errno=EINVAL;
    return bad ? -1 : 0;
}

static BOOL provider_read(HANDLE file,void *buffer,size_t size,uint64_t at,size_t *received) {
    LARGE_INTEGER length;
    *received=0;
    if (!GetFileSizeEx(file,&length) || length.QuadPart<0) return FALSE;
    uint64_t available=at<(uint64_t)length.QuadPart ? (uint64_t)length.QuadPart-at : 0;
    if (size>available) size=(size_t)available;
    /* Even EOF must validate the handle; otherwise an unknown input can appear
     * to be an empty valid file. The callback always validates offset and file. */
    if (input_read(input_context,file,buffer,size,at)) {
        InterlockedExchange(&input_failed,1);
        SetLastError(ERROR_READ_FAULT);return FALSE;
    }
    *received=size;return TRUE;
}

#ifndef LUMABRI_P2P
#define LUMABRI_P2P
#endif
#include "lumi_v4_ext.h"
static void *home_expert_context;
static LmbHomeExpertFn home_expert_function;
void lmb_home_expert_provider(void *context, LmbHomeExpertFn function) {
    home_expert_context=context; home_expert_function=function;
}
int lmb_home_expert_available(void) { return home_expert_function!=NULL; }
int lmb_home_expert_apply(int layer,int expert,const float *input,int width,float *output) {
    return home_expert_function ? home_expert_function(home_expert_context,layer,expert,input,width,output) : -1;
}

/* These are the same preparation hooks used by the POSIX bridge. Each model
 * process has its own input lifetime; there is deliberately no unseal API. */
void lmb_resident_adapter_prepared(void) { InterlockedExchange(&prepared,1); }
int lmb_resident_adapter_is_prepared(void) { return InterlockedCompareExchange(&prepared,0,0)!=0; }
int lmb_resident_budget_exceeded(unsigned long long limit) {
    LmbProcessMemory process;
    PROCESS_MEMORY_COUNTERS peak={0};peak.cb=sizeof peak;
    return !limit || lmb_process_memory_probe(&process,0) ||
        !GetProcessMemoryInfo(GetCurrentProcess(),&peak,sizeof peak) ||
        lmb_process_memory_charge(&process)>limit || peak.PeakWorkingSetSize>limit;
}

BOOL WINAPI lmb_engine_ReadFile(HANDLE file,LPVOID buffer,DWORD size,
                                LPDWORD received,LPOVERLAPPED offset) {
    AcquireSRWLockShared(&input_lock);
    if(input_sealed) {
        InterlockedIncrement64(&input_denied);
        if(received) *received=0;
        ReleaseSRWLockShared(&input_lock);
        SetLastError(ERROR_ACCESS_DENIED); return FALSE;
    }
    DWORD count=0;
    BOOL ok;
    if (input_read) {
        LARGE_INTEGER at={0},zero={0};size_t n=0;
        ok=offset || SetFilePointerEx(file,zero,&at,FILE_CURRENT);
        uint64_t position=offset ? ((uint64_t)offset->OffsetHigh<<32)|offset->Offset : (uint64_t)at.QuadPart;
        if (ok) ok=provider_read(file,buffer,size,position,&n);
        count=(DWORD)n;
        if (ok && !offset) {at.QuadPart=(LONGLONG)(position+n);ok=SetFilePointerEx(file,at,NULL,FILE_BEGIN);}
        if (!ok) InterlockedExchange(&input_failed,1);
    } else ok=ReadFile(file,buffer,size,&count,offset);
    /* Seal must wait for every read, including a native handle opened for
     * overlapped input. No pending read may outlive this shared lock. */
    if(!ok && offset && GetLastError()==ERROR_IO_PENDING)
        ok=GetOverlappedResult(file,offset,&count,TRUE);
    DWORD error=ok ? ERROR_SUCCESS : GetLastError();
    if(received) *received=count;
    InterlockedIncrement64(&input_calls);
    InterlockedAdd64(&input_bytes,count);
    ReleaseSRWLockShared(&input_lock);
    SetLastError(error); return ok;
}
size_t lmb_engine_fread(void *buffer,size_t size,size_t count,FILE *file) {
    if(!size || !count) return 0;
    if(size && count>SIZE_MAX/size) { errno=EOVERFLOW; return 0; }
    AcquireSRWLockShared(&input_lock);
    if(input_sealed) {
        InterlockedIncrement64(&input_denied);
        ReleaseSRWLockShared(&input_lock); errno=EACCES; return 0;
    }
    size_t read;
    if (input_read) {
        __int64 at=_ftelli64(file);size_t bytes=0;
        HANDLE handle=(HANDLE)_get_osfhandle(_fileno(file));
        if (at<0 || !provider_read(handle,buffer,size*count,(uint64_t)at,&bytes) ||
            _fseeki64(file,at+(__int64)bytes,SEEK_SET)) {InterlockedExchange(&input_failed,1);errno=EIO;read=0;}
        else read=bytes/size;
    } else read=fread(buffer,size,count,file);
    InterlockedIncrement64(&input_calls);
    InterlockedAdd64(&input_bytes,(LONG64)(read*size));
    ReleaseSRWLockShared(&input_lock); return read;
}
int lmb_engine_input_seal(void) {
    AcquireSRWLockExclusive(&input_lock);
    LmbEngineMemory memory;
    lmb_engine_memory_snapshot(&memory);
    int valid=lmb_resident_adapter_is_prepared() && memory.enabled &&
        !memory.fault && memory.allocations && memory.locked_bytes &&
        !InterlockedCompareExchange(&input_failed,0,0);
    if(valid) {
        input_sealed=1;
        /* All shared reads have drained; release preparation buffers and
         * transport handles before announcing resident readiness. */
        if(input_close) {input_close(input_context);input_close=NULL;input_context=NULL;}
    }
    ReleaseSRWLockExclusive(&input_lock);
    if(!valid) errno=EPERM;
    return valid ? 0 : -1;
}
int lmb_engine_input_lock_status(uint64_t *retained_bytes) {
    /* Native input owns no retained file mappings. Every prepared tensor is
     * covered by the engine allocator; post-seal input attempts are faults. */
    if(!retained_bytes) { errno=EINVAL; return -1; }
    *retained_bytes=0;
    AcquireSRWLockShared(&input_lock);
    int valid=input_sealed && !InterlockedCompareExchange64(&input_denied,0,0);
    ReleaseSRWLockShared(&input_lock);
    if(!valid) errno=EPERM;
    return valid ? 0 : -1;
}
void lmb_engine_input_snapshot(LmbEngineInput *out) {
    if(!out) return;
    AcquireSRWLockExclusive(&input_lock);
    out->read_calls=(uint64_t)input_calls;
    out->read_bytes=(uint64_t)input_bytes;
    out->denied_reads=(uint64_t)input_denied;
    out->sealed=input_sealed;
    ReleaseSRWLockExclusive(&input_lock);
}
