/* Native operations beneath the owned-allocation ledger. No policy changes:
 * callers retain responsibility for the approved memory/working-set budget. */
#ifndef LMB_ENGINE_MEMORY_OS_H
#define LMB_ENGINE_MEMORY_OS_H
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
/* CRT malloc alignment comes from the native ABI; MSVC does not expose
 * max_align_t in every C11 SDK combination. */
#define LMB_MEMORY_MALLOC_ALIGNMENT MEMORY_ALLOCATION_ALIGNMENT
#ifndef ESTALE
#define ESTALE EIO
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)(intptr_t)-1)
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED 0x10
#define PROT_READ 1
#define PROT_WRITE 2
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#endif
static inline int lmb_memory_os_error(void) {
    DWORD error=GetLastError();
    errno=error==ERROR_ACCESS_DENIED ? EACCES :
        error==ERROR_INVALID_PARAMETER || error==ERROR_INVALID_ADDRESS ? EINVAL : ENOMEM;
    return -1;
}
static inline uint64_t lmb_memory_pid(void) { return GetCurrentProcessId(); }
static inline size_t lmb_memory_pagesize(void) {
    SYSTEM_INFO info; GetSystemInfo(&info); return info.dwPageSize;
}
static inline int lmb_memory_lock(const void *ptr,size_t size) {
    return VirtualLock((void *)ptr,size) ? 0 : lmb_memory_os_error();
}
static inline int lmb_memory_unlock(const void *ptr,size_t size) {
    return VirtualUnlock((void *)ptr,size) ? 0 : lmb_memory_os_error();
}
static inline int lmb_memory_aligned(void **ptr,size_t alignment,size_t size) {
    void *value=_aligned_malloc(size,alignment);
    if (!value) return ENOMEM;
    *ptr=value; return 0;
}
static inline void lmb_memory_release(void *ptr,int aligned) {
    if (aligned) _aligned_free(ptr); else free(ptr);
}
static inline void *lmb_memory_map(void *addr,size_t bytes,int prot,int flags,int fd,off_t offset) {
    if (!bytes || flags!=(MAP_ANONYMOUS|MAP_PRIVATE) || fd!=-1 || offset ||
        (prot!=PROT_READ && prot!=(PROT_READ|PROT_WRITE))) {
        errno=ENOTSUP; return MAP_FAILED;
    }
    void *ptr=VirtualAlloc(addr,bytes,MEM_COMMIT|MEM_RESERVE,
                          prot&PROT_WRITE ? PAGE_READWRITE : PAGE_READONLY);
    if (!ptr) { (void)lmb_memory_os_error(); return MAP_FAILED; }
    return ptr;
}
static inline int lmb_memory_unmap(void *ptr,size_t bytes) {
    MEMORY_BASIC_INFORMATION info;
    size_t page=lmb_memory_pagesize();
    if (!bytes || bytes>SIZE_MAX-(page-1) ||
        !VirtualQuery(ptr,&info,sizeof info) || info.AllocationBase!=ptr ||
        info.Type!=MEM_PRIVATE || info.RegionSize!=((bytes+page-1)&~(page-1))) {
        errno=EINVAL; return -1;
    }
    return VirtualFree(ptr,0,MEM_RELEASE) ? 0 : lmb_memory_os_error();
}
static inline int lmb_memory_advise(void *ptr,size_t bytes,int advice) {
    (void)ptr; (void)bytes;
    if (advice>=MADV_NORMAL && advice<=MADV_WILLNEED) return 0;
    errno=ENOTSUP; return -1;
}
static inline int lmb_memory_lockall(int flags) { (void)flags; errno=ENOTSUP; return -1; }
static inline int lmb_memory_unlockall(void) { errno=ENOTSUP; return -1; }
#else
#include <unistd.h>
#include <sys/mman.h>
#define LMB_MEMORY_MALLOC_ALIGNMENT _Alignof(max_align_t)
static inline uint64_t lmb_memory_pid(void) { return (uint64_t)getpid(); }
static inline size_t lmb_memory_pagesize(void) {
    long page=sysconf(_SC_PAGESIZE); return page>0 ? (size_t)page : 0;
}
static inline int lmb_memory_lock(const void *ptr,size_t size) { return mlock(ptr,size); }
static inline int lmb_memory_unlock(const void *ptr,size_t size) { return munlock(ptr,size); }
static inline int lmb_memory_aligned(void **ptr,size_t alignment,size_t size) { return posix_memalign(ptr,alignment,size); }
static inline void lmb_memory_release(void *ptr,int aligned) { (void)aligned; free(ptr); }
static inline void *lmb_memory_map(void *addr,size_t size,int prot,int flags,int fd,off_t offset) { return mmap(addr,size,prot,flags,fd,offset); }
static inline int lmb_memory_unmap(void *ptr,size_t size) { return munmap(ptr,size); }
static inline int lmb_memory_advise(void *ptr,size_t size,int advice) { return madvise(ptr,size,advice); }
static inline int lmb_memory_lockall(int flags) { return mlockall(flags); }
static inline int lmb_memory_unlockall(void) { return munlockall(); }
#endif
#endif
