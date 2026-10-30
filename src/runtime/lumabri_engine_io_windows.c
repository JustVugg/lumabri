#include "lumabri_engine_io_windows.h"
#include "lumabri_engine_memory.h"
#include "lumabri_process_memory.h"
#include <errno.h>

static SRWLOCK input_lock=SRWLOCK_INIT;
static volatile LONG64 input_calls,input_bytes,input_denied;
static int input_sealed;
static volatile LONG prepared;

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
    BOOL ok=ReadFile(file,buffer,size,&count,offset);
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
    size_t read=fread(buffer,size,count,file);
    InterlockedIncrement64(&input_calls);
    InterlockedAdd64(&input_bytes,(LONG64)(read*size));
    ReleaseSRWLockShared(&input_lock); return read;
}
int lmb_engine_input_seal(void) {
    AcquireSRWLockExclusive(&input_lock);
    LmbEngineMemory memory;
    lmb_engine_memory_snapshot(&memory);
    int valid=lmb_resident_adapter_is_prepared() && memory.enabled &&
        !memory.fault && memory.allocations && memory.locked_bytes;
    if(valid) input_sealed=1;
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
