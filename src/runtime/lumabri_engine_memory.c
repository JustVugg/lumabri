/* Compile WITHOUT the override header. Ordinary payloads keep the libc ABI.
 * Heap objects share reference-counted locked pages. Dedicated aligned
 * extents have their own locks. Freeing one cannot unlock another.
 * This allocator never raises a resource limit or changes machine policy. */
#define _GNU_SOURCE
#include "lumabri_engine_memory.h"
#include "lumabri_memlock.h"
#include "lumabri_engine_memory_os.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define LMB_ENGINE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LMB_ENGINE_ASAN 1
#endif
#endif

#define LMB_ENGINE_BUCKETS 1024u
typedef struct Allocation {
    void *ptr;
    size_t requested, extent;
    int mapping, shared_pages, native_aligned, locked;
    struct Allocation *next;
} Allocation;
typedef struct LockedPage {
    uintptr_t address;
    size_t references;
    struct LockedPage *next;
} LockedPage;
#ifdef _WIN32
static SRWLOCK allocation_lock=SRWLOCK_INIT;
static INIT_ONCE configure_once=INIT_ONCE_STATIC_INIT;
static void lock_allocations(void) { AcquireSRWLockExclusive(&allocation_lock); }
static void unlock_allocations(void) { ReleaseSRWLockExclusive(&allocation_lock); }
#else
static pthread_mutex_t allocation_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t configure_once=PTHREAD_ONCE_INIT;
static void lock_allocations(void) { pthread_mutex_lock(&allocation_lock); }
static void unlock_allocations(void) { pthread_mutex_unlock(&allocation_lock); }
#endif
static Allocation *allocations[LMB_ENGINE_BUCKETS];
static LockedPage *locked_pages[LMB_ENGINE_BUCKETS];
static LmbEngineMemory evidence;
static size_t page_size;
static uint64_t owner_pid;
#ifndef _WIN32
static void before_fork(void) { lock_allocations(); }
static void after_fork(void) { unlock_allocations(); }
static void child_fork(void) {
    /* POSIX locks are not inherited. Keep the ledger for safe destruction,
     * but never report the parent's locks as evidence in the child. */
    if (evidence.enabled) evidence.fault=ESTALE;
    unlock_allocations();
}
#endif

static void configure(void) {
    const char *mode=getenv("LUMABRI_RESIDENCY_POLICY");
    evidence.enabled=mode && !strcmp(mode,"engine-locked");
    owner_pid=lmb_memory_pid();
    size_t page=lmb_memory_pagesize();
    if (!page || (page&(page-1))) evidence.fault=EINVAL;
    else page_size=page;
#ifndef _WIN32
    if (evidence.enabled) {
        int rc=pthread_atfork(before_fork,after_fork,child_fork);
        if (rc) evidence.fault=rc;
    }
#endif
#ifdef LMB_ENGINE_ASAN
    /* Sanitizers can turn mlock into a successful no-op. Never certify that. */
    if (evidence.enabled) evidence.fault=ENOTSUP;
#endif
}
#ifdef _WIN32
static BOOL CALLBACK configure_native(PINIT_ONCE once,PVOID argument,PVOID *context) {
    (void)once; (void)argument; (void)context; configure(); return TRUE;
}
#endif
static int enabled(void) {
#ifdef _WIN32
    InitOnceExecuteOnce(&configure_once,configure_native,NULL,NULL);
#else
    pthread_once(&configure_once,configure);
#endif
    return evidence.enabled;
}
static size_t bucket(const void *ptr) {
    return ((uintptr_t)ptr>>12)%LMB_ENGINE_BUCKETS;
}
static Allocation **lookup(void *ptr) {
    Allocation **at=&allocations[bucket(ptr)];
    while (*at && (*at)->ptr!=ptr) at=&(*at)->next;
    return at;
}
/* Only uncommon VM operations need an interval scan. The hot malloc/free
 * path uses the pointer hash. An interior pointer must not bypass ownership. */
static Allocation *overlap(const void *ptr, size_t bytes) {
    uintptr_t begin=(uintptr_t)ptr;
    if (!bytes) return NULL;
    uintptr_t end=bytes>UINTPTR_MAX-begin ? UINTPTR_MAX : begin+bytes;
    for (unsigned i=0;i<LMB_ENGINE_BUCKETS;i++)
        for (Allocation *p=allocations[i];p;p=p->next) {
            uintptr_t low=(uintptr_t)p->ptr;
            uintptr_t high=low+p->extent;
            /* VM operations round to page boundaries, including the libc
             * neighbours of small engine objects. Protect those pages too. */
            if (p->shared_pages) {
                low&=~(uintptr_t)(page_size-1);
                high=(high+page_size-1)&~(uintptr_t)(page_size-1);
            }
            if (begin<high && end>low) return p;
        }
    return NULL;
}
static int fault_locked(int error) {
    if (!evidence.fault) evidence.fault=error ? error : EIO;
    errno=evidence.fault;
    return -1;
}
static int extent_for(size_t bytes, size_t *extent) {
    if (!bytes) bytes=1;
    if (!page_size || bytes>SIZE_MAX-(page_size-1)) { errno=ENOMEM; return -1; }
    *extent=(bytes+page_size-1)&~(page_size-1);
    return 0;
}

static LockedPage **page_lookup(uintptr_t address) {
    LockedPage **at=&locked_pages[bucket((void *)address)];
    while (*at && (*at)->address!=address) at=&(*at)->next;
    return at;
}
/* allocation_lock covers both ledgers and all OS lock transitions. */
static void release_page(uintptr_t address) {
    LockedPage **at=page_lookup(address), *entry=*at;
    if (!entry || !entry->references) { (void)fault_locked(EINVAL); return; }
    if (--entry->references) return;
    if (lmb_memory_unlock((void *)address,page_size)) (void)fault_locked(errno);
    *at=entry->next;
    evidence.locked_bytes-=page_size;
    free(entry);
}
static void release_pages(void *ptr, size_t extent) {
    uintptr_t first=(uintptr_t)ptr&~(uintptr_t)(page_size-1);
    uintptr_t last=((uintptr_t)ptr+extent-1)&~(uintptr_t)(page_size-1);
    for (uintptr_t page=first;;page+=page_size) {
        release_page(page);
        if (page==last) break;
    }
}
static int retain_pages(void *ptr, size_t extent) {
    uintptr_t first=(uintptr_t)ptr&~(uintptr_t)(page_size-1);
    uintptr_t last=((uintptr_t)ptr+extent-1)&~(uintptr_t)(page_size-1);
    for (uintptr_t page=first;;page+=page_size) {
        LockedPage **at=page_lookup(page), *entry=*at;
        int rc=0;
        if (entry) {
            if (entry->references==SIZE_MAX) rc=ENOMEM;
            else entry->references++;
        } else {
            entry=calloc(1,sizeof *entry);
            if (!entry || evidence.locked_bytes>UINT64_MAX-page_size) rc=ENOMEM;
            else if (lmb_memory_lock((void *)page,page_size)) {
                rc=errno;
                lmb_memlock_failure(page_size,evidence.locked_bytes,rc);
            }
            if (rc) free(entry);
            else {
                entry->address=page; entry->references=1;
                *at=entry;
                evidence.locked_bytes+=page_size;
            }
        }
        if (rc) {
            for (uintptr_t done=first;done!=page;done+=page_size) release_page(done);
            return rc;
        }
        if (page==last) return 0;
    }
}

void lmb_engine_memory_snapshot(LmbEngineMemory *out) {
    if (!out) return;
    (void)enabled();
    lock_allocations();
    /* Lock ownership is not inherited across fork. Do not report the parent's
     * counters as proof in a child. Dedicated model processes normally exec. */
    if (evidence.enabled && owner_pid!=lmb_memory_pid()) (void)fault_locked(ESTALE);
    *out=evidence;
    unlock_allocations();
}

static void *allocate(size_t bytes, size_t alignment) {
    size_t extent;
    if (extent_for(bytes,&extent)) return NULL;
    int shared_pages=bytes<=page_size/2 && alignment<page_size;
#ifdef _WIN32
    /* Windows' aligned CRT allocator requires its matching free. Keep all
     * ordinary malloc/calloc/realloc payloads on the ordinary CRT heap;
     * reference-count their pages even for large objects. Only explicit
     * alignment requests can use the aligned CRT allocation family. */
    if (alignment<=LMB_MEMORY_MALLOC_ALIGNMENT) shared_pages=1;
#endif
    if (shared_pages) extent=bytes ? bytes : 1;
    else if (alignment<page_size) alignment=page_size;
    lock_allocations();
    if (evidence.fault || owner_pid!=lmb_memory_pid() ||
        evidence.requested_bytes>UINT64_MAX-bytes || evidence.locked_bytes>UINT64_MAX-extent) {
        (void)fault_locked(ENOMEM); unlock_allocations(); return NULL;
    }
    Allocation *entry=calloc(1,sizeof *entry);
    void *ptr=NULL;
    int rc=ENOMEM;
    if (entry) {
        if (shared_pages && alignment<=LMB_MEMORY_MALLOC_ALIGNMENT) {
            ptr=malloc(extent); rc=ptr ? 0 : ENOMEM;
        } else { rc=lmb_memory_aligned(&ptr,alignment,extent); entry->native_aligned=1; }
    }
    if (!rc && evidence.enabled && shared_pages) rc=retain_pages(ptr,extent);
    else if (!rc && evidence.enabled && lmb_memory_lock(ptr,extent)) {
        rc=errno;
        lmb_memlock_failure(extent,evidence.locked_bytes,rc);
    }
    if (rc) {
        lmb_memory_release(ptr,entry ? entry->native_aligned : 0);
        free(entry); (void)fault_locked(rc);
        unlock_allocations(); return NULL;
    }
    entry->ptr=ptr; entry->extent=extent; entry->requested=bytes;
    entry->shared_pages=shared_pages;
    entry->locked=evidence.enabled;
    entry->next=allocations[bucket(ptr)]; allocations[bucket(ptr)]=entry;
    evidence.allocations++; evidence.requested_bytes+=bytes;
    if (entry->locked && !shared_pages) evidence.locked_bytes+=extent;
    unlock_allocations();
    return ptr;
}

void *lmb_engine_malloc(size_t bytes) {
    if (!enabled()) return malloc(bytes);
    return allocate(bytes,LMB_MEMORY_MALLOC_ALIGNMENT);
}
void *lmb_engine_calloc(size_t count, size_t bytes) {
    if (!enabled()) return calloc(count,bytes);
    if (bytes && count>SIZE_MAX/bytes) { errno=ENOMEM; return NULL; }
    size_t total=count*bytes;
    void *ptr=allocate(total,LMB_MEMORY_MALLOC_ALIGNMENT);
    if (ptr) memset(ptr,0,total);
    return ptr;
}
void lmb_engine_free(void *ptr) {
    if (!ptr) return;
#ifndef _WIN32
    if (!enabled()) { free(ptr); return; }
#else
    (void)enabled(); /* also owns Windows' explicit aligned CRT allocations */
#endif
    lock_allocations();
    Allocation **at=lookup(ptr), *entry=*at;
    if (!entry) { unlock_allocations(); free(ptr); return; }
    if (entry->mapping) { (void)fault_locked(EINVAL); unlock_allocations(); return; }
    *at=entry->next;
    if (entry->locked && entry->shared_pages) release_pages(ptr,entry->extent);
    else if (entry->locked) {
        if (lmb_memory_unlock(ptr,entry->extent)) (void)fault_locked(errno);
        evidence.locked_bytes-=entry->extent;
    }
    evidence.allocations--; evidence.requested_bytes-=entry->requested;
    lmb_memory_release(ptr,entry->native_aligned); free(entry);
    unlock_allocations();
}
void *lmb_engine_realloc(void *ptr, size_t bytes) {
#ifndef _WIN32
    if (!enabled()) return realloc(ptr,bytes);
#else
    (void)enabled();
#endif
    if (!ptr) return lmb_engine_malloc(bytes);
    if (!bytes) { lmb_engine_free(ptr); return NULL; }
    lock_allocations();
    Allocation *entry=*lookup(ptr);
#ifdef _WIN32
    if (!entry && !evidence.enabled) { unlock_allocations(); return realloc(ptr,bytes); }
#endif
    if (!entry || entry->mapping) {
        (void)fault_locked(EINVAL); unlock_allocations(); return NULL;
    }
    size_t previous=entry->requested;
    unlock_allocations();
    /* Realloc's caller owns the pointer exclusively, as with libc. A failed
     * replacement leaves the old payload and its lock intact. */
    void *next=allocate(bytes,LMB_MEMORY_MALLOC_ALIGNMENT);
    if (next) { memcpy(next,ptr,previous<bytes ? previous : bytes); lmb_engine_free(ptr); }
    return next;
}
int lmb_engine_posix_memalign(void **ptr, size_t alignment, size_t bytes) {
#ifndef _WIN32
    if (!enabled()) return posix_memalign(ptr,alignment,bytes);
#else
    (void)enabled();
#endif
    if (!alignment || (alignment&(alignment-1)) || alignment%sizeof(void *)) return EINVAL;
    void *p=allocate(bytes,alignment);
    if (!p) return errno ? errno : ENOMEM;
    *ptr=p; return 0;
}
void *lmb_engine_aligned_alloc(size_t alignment, size_t bytes) {
#ifndef _WIN32
    if (!enabled()) return aligned_alloc(alignment,bytes);
#else
    (void)enabled();
#endif
    if (!alignment || (alignment&(alignment-1)) || bytes%alignment) { errno=EINVAL; return NULL; }
    return allocate(bytes,alignment);
}
char *lmb_engine_strndup(const char *text, size_t max) {
    size_t size=strnlen(text,max);
    if (size==SIZE_MAX) { errno=ENOMEM; return NULL; }
    char *out=lmb_engine_malloc(size+1);
    if (out) { memcpy(out,text,size); out[size]=0; }
    return out;
}
char *lmb_engine_strdup(const char *text) {
    return lmb_engine_strndup(text,SIZE_MAX);
}
#ifdef _WIN32
void *lmb_engine_win_aligned_malloc(size_t bytes,size_t alignment) {
    if (alignment<sizeof(void *)) alignment=sizeof(void *);
    void *ptr=NULL;
    int rc=lmb_engine_posix_memalign(&ptr,alignment,bytes);
    if (rc) errno=rc;
    return ptr;
}
void lmb_engine_win_aligned_free(void *ptr) {
    if (!ptr) return;
    (void)enabled();
    lock_allocations(); int owned=*lookup(ptr)!=NULL; unlock_allocations();
    if (owned) lmb_engine_free(ptr); else _aligned_free(ptr);
}
#endif
void *lmb_engine_mmap(void *addr, size_t bytes, int prot, int flags, int fd, off_t offset) {
    if (!enabled()) return lmb_memory_map(addr,bytes,prot,flags,fd,offset);
    /* File mappings are not resident tensor ownership. Refuse them instead
     * of certifying an untracked fallback; callers may use their read path. */
    if (!bytes || !(flags&MAP_ANONYMOUS) || (flags&MAP_FIXED) || fd!=-1 || offset) {
        errno=ENOTSUP; return MAP_FAILED;
    }
    size_t extent;
    if (extent_for(bytes,&extent)) return MAP_FAILED;
    lock_allocations();
    if (evidence.fault || owner_pid!=lmb_memory_pid() ||
        evidence.requested_bytes>UINT64_MAX-bytes || evidence.locked_bytes>UINT64_MAX-extent) {
        (void)fault_locked(ENOMEM); unlock_allocations(); return MAP_FAILED;
    }
    Allocation *entry=calloc(1,sizeof *entry);
    void *ptr=entry ? lmb_memory_map(addr,bytes,prot,flags,fd,offset) : MAP_FAILED;
    int rc=!entry ? ENOMEM : ptr==MAP_FAILED ? errno : 0;
    if (!rc && lmb_memory_lock(ptr,extent)) {
        rc=errno;
        lmb_memlock_failure(extent,evidence.locked_bytes,rc);
    }
    if (rc) {
        if (ptr!=MAP_FAILED) (void)lmb_memory_unmap(ptr,extent);
        free(entry); (void)fault_locked(rc); unlock_allocations(); return MAP_FAILED;
    }
    entry->ptr=ptr; entry->requested=bytes; entry->extent=extent; entry->mapping=1;
    entry->next=allocations[bucket(ptr)]; allocations[bucket(ptr)]=entry;
    evidence.allocations++; evidence.requested_bytes+=bytes; evidence.locked_bytes+=extent;
    unlock_allocations(); return ptr;
}
int lmb_engine_munmap(void *ptr, size_t bytes) {
    if (!enabled()) return lmb_memory_unmap(ptr,bytes);
    lock_allocations();
    Allocation **at=lookup(ptr), *entry=*at;
    if (!entry) {
        int owned=overlap(ptr,bytes)!=NULL;
        unlock_allocations();
        if (owned) { errno=EINVAL; return -1; }
        return lmb_memory_unmap(ptr,bytes);
    }
    if (!entry->mapping || (bytes!=entry->requested && bytes!=entry->extent)) {
        unlock_allocations(); errno=EINVAL; return -1;
    }
    if (lmb_memory_unmap(ptr,bytes)) { unlock_allocations(); return -1; }
    *at=entry->next;
    evidence.allocations--; evidence.requested_bytes-=entry->requested; evidence.locked_bytes-=entry->extent;
    free(entry); unlock_allocations(); return 0;
}

static int owned_range(const void *ptr, size_t bytes) {
    if (!bytes) return 0;
    Allocation *exact=*lookup((void *)ptr);
    if (exact && bytes<=exact->extent) return 1;
    uintptr_t p=(uintptr_t)ptr;
    /* Several small allocations may share a protected page. The first page
     * overlap need not be the object actually containing this range. */
    for (unsigned i=0;i<LMB_ENGINE_BUCKETS;i++)
        for (Allocation *entry=allocations[i];entry;entry=entry->next) {
            uintptr_t low=(uintptr_t)entry->ptr;
            if (p>=low && p-low<=entry->extent && bytes<=entry->extent-(p-low)) return 1;
        }
    return overlap(ptr,bytes) ? -1 : 0;
}
int lmb_engine_mlock(const void *ptr, size_t bytes) {
    if (!enabled()) return lmb_memory_lock(ptr,bytes);
    lock_allocations();
    int owned=owned_range(ptr,bytes);
    unlock_allocations();
    if (owned<0) { errno=EPERM; return -1; }
    return owned ? 0 : lmb_memory_lock(ptr,bytes);
}
int lmb_engine_munlock(const void *ptr, size_t bytes) {
    if (!enabled()) return lmb_memory_unlock(ptr,bytes);
    lock_allocations();
    int owned=owned_range(ptr,bytes);
    unlock_allocations();
    if (owned<0) { errno=EPERM; return -1; }
    /* An upstream cache's optional pin is subordinate to resident ownership.
     * Release the one OS lock only when this allocation is actually freed. */
    return owned ? 0 : lmb_memory_unlock(ptr,bytes);
}
int lmb_engine_mlockall(int flags) {
    if (!enabled()) return lmb_memory_lockall(flags);
    errno=ENOTSUP; return -1; /* no whole-process locking claim */
}
int lmb_engine_munlockall(void) {
    if (!enabled()) return lmb_memory_unlockall();
    errno=EPERM; return -1;
}
int lmb_engine_madvise(void *ptr, size_t bytes, int advice) {
    if (!enabled()) return lmb_memory_advise(ptr,bytes,advice);
    lock_allocations();
    int owned=overlap(ptr,bytes)!=NULL;
    unlock_allocations();
    if (owned) {
        if (advice==MADV_NORMAL || advice==MADV_RANDOM || advice==MADV_SEQUENTIAL || advice==MADV_WILLNEED) return 0;
        errno=EPERM; return -1;
    }
    return lmb_memory_advise(ptr,bytes,advice);
}
