/* Compile WITHOUT the override header. Payloads keep the libc free ABI.
 * Small objects share reference-counted locked pages; large/aligned objects
 * have disjoint page-aligned extents. Freeing one cannot unlock another.
 * This allocator never raises a resource limit or changes machine policy. */
#define _GNU_SOURCE
#include "lumabri_engine_memory.h"
#include "lumabri_memlock.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

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
    int mapping, shared_pages;
    struct Allocation *next;
} Allocation;
typedef struct LockedPage {
    uintptr_t address;
    size_t references;
    struct LockedPage *next;
} LockedPage;
static pthread_mutex_t allocation_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t configure_once=PTHREAD_ONCE_INIT;
static Allocation *allocations[LMB_ENGINE_BUCKETS];
static LockedPage *locked_pages[LMB_ENGINE_BUCKETS];
static LmbEngineMemory evidence;
static size_t page_size;
static pid_t owner_pid;
static void before_fork(void) { pthread_mutex_lock(&allocation_lock); }
static void after_fork(void) { pthread_mutex_unlock(&allocation_lock); }
static void child_fork(void) {
    /* POSIX locks are not inherited. Keep the ledger for safe destruction,
     * but never report the parent's locks as evidence in the child. */
    if (evidence.enabled) evidence.fault=ESTALE;
    pthread_mutex_unlock(&allocation_lock);
}

static void configure(void) {
    const char *mode=getenv("LUMABRI_RESIDENCY_POLICY");
    evidence.enabled=mode && !strcmp(mode,"engine-locked");
    owner_pid=getpid();
    long page=sysconf(_SC_PAGESIZE);
    if (page<=0 || ((size_t)page&((size_t)page-1))) evidence.fault=EINVAL;
    else page_size=(size_t)page;
    if (evidence.enabled) {
        int rc=pthread_atfork(before_fork,after_fork,child_fork);
        if (rc) evidence.fault=rc;
    }
#ifdef LMB_ENGINE_ASAN
    /* Sanitizers can turn mlock into a successful no-op. Never certify that. */
    if (evidence.enabled) evidence.fault=ENOTSUP;
#endif
}
static int enabled(void) {
    pthread_once(&configure_once,configure);
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
    if (munlock((void *)address,page_size)) (void)fault_locked(errno);
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
            else if (mlock((void *)page,page_size)) {
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
    pthread_mutex_lock(&allocation_lock);
    /* Lock ownership is not inherited across fork. Do not report the parent's
     * counters as proof in a child. Dedicated model processes normally exec. */
    if (evidence.enabled && owner_pid!=getpid()) (void)fault_locked(ESTALE);
    *out=evidence;
    pthread_mutex_unlock(&allocation_lock);
}

static void *allocate(size_t bytes, size_t alignment) {
    size_t extent;
    if (extent_for(bytes,&extent)) return NULL;
    int shared_pages=bytes<=page_size/2 && alignment<page_size;
    if (shared_pages) extent=bytes ? bytes : 1;
    else if (alignment<page_size) alignment=page_size;
    pthread_mutex_lock(&allocation_lock);
    if (evidence.fault || owner_pid!=getpid() ||
        evidence.requested_bytes>UINT64_MAX-bytes || evidence.locked_bytes>UINT64_MAX-extent) {
        (void)fault_locked(ENOMEM); pthread_mutex_unlock(&allocation_lock); return NULL;
    }
    Allocation *entry=calloc(1,sizeof *entry);
    void *ptr=NULL;
    int rc=ENOMEM;
    if (entry) {
        if (shared_pages && alignment<=_Alignof(max_align_t)) {
            ptr=malloc(extent); rc=ptr ? 0 : ENOMEM;
        } else rc=posix_memalign(&ptr,alignment,extent);
    }
    if (!rc && shared_pages) rc=retain_pages(ptr,extent);
    else if (!rc && mlock(ptr,extent)) {
        rc=errno;
        lmb_memlock_failure(extent,evidence.locked_bytes,rc);
    }
    if (rc) {
        free(ptr); free(entry); (void)fault_locked(rc);
        pthread_mutex_unlock(&allocation_lock); return NULL;
    }
    entry->ptr=ptr; entry->extent=extent; entry->requested=bytes;
    entry->shared_pages=shared_pages;
    entry->next=allocations[bucket(ptr)]; allocations[bucket(ptr)]=entry;
    evidence.allocations++; evidence.requested_bytes+=bytes;
    if (!shared_pages) evidence.locked_bytes+=extent;
    pthread_mutex_unlock(&allocation_lock);
    return ptr;
}

void *lmb_engine_malloc(size_t bytes) {
    if (!enabled()) return malloc(bytes);
    return allocate(bytes,_Alignof(max_align_t));
}
void *lmb_engine_calloc(size_t count, size_t bytes) {
    if (!enabled()) return calloc(count,bytes);
    if (bytes && count>SIZE_MAX/bytes) { errno=ENOMEM; return NULL; }
    size_t total=count*bytes;
    void *ptr=allocate(total,_Alignof(max_align_t));
    if (ptr) memset(ptr,0,total);
    return ptr;
}
void lmb_engine_free(void *ptr) {
    if (!ptr) return;
    if (!enabled()) { free(ptr); return; }
    pthread_mutex_lock(&allocation_lock);
    Allocation **at=lookup(ptr), *entry=*at;
    if (!entry) { pthread_mutex_unlock(&allocation_lock); free(ptr); return; }
    if (entry->mapping) { (void)fault_locked(EINVAL); pthread_mutex_unlock(&allocation_lock); return; }
    *at=entry->next;
    if (entry->shared_pages) release_pages(ptr,entry->extent);
    else {
        if (munlock(ptr,entry->extent)) (void)fault_locked(errno);
        evidence.locked_bytes-=entry->extent;
    }
    evidence.allocations--; evidence.requested_bytes-=entry->requested;
    free(ptr); free(entry);
    pthread_mutex_unlock(&allocation_lock);
}
void *lmb_engine_realloc(void *ptr, size_t bytes) {
    if (!enabled()) return realloc(ptr,bytes);
    if (!ptr) return lmb_engine_malloc(bytes);
    if (!bytes) { lmb_engine_free(ptr); return NULL; }
    pthread_mutex_lock(&allocation_lock);
    Allocation *entry=*lookup(ptr);
    if (!entry || entry->mapping) {
        (void)fault_locked(EINVAL); pthread_mutex_unlock(&allocation_lock); return NULL;
    }
    size_t previous=entry->requested;
    pthread_mutex_unlock(&allocation_lock);
    /* Realloc's caller owns the pointer exclusively, as with libc. A failed
     * replacement leaves the old payload and its lock intact. */
    void *next=allocate(bytes,_Alignof(max_align_t));
    if (next) { memcpy(next,ptr,previous<bytes ? previous : bytes); lmb_engine_free(ptr); }
    return next;
}
int lmb_engine_posix_memalign(void **ptr, size_t alignment, size_t bytes) {
    if (!enabled()) return posix_memalign(ptr,alignment,bytes);
    if (!alignment || (alignment&(alignment-1)) || alignment%sizeof(void *)) return EINVAL;
    void *p=allocate(bytes,alignment);
    if (!p) return errno ? errno : ENOMEM;
    *ptr=p; return 0;
}
void *lmb_engine_aligned_alloc(size_t alignment, size_t bytes) {
    if (!enabled()) return aligned_alloc(alignment,bytes);
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
void *lmb_engine_mmap(void *addr, size_t bytes, int prot, int flags, int fd, off_t offset) {
    if (!enabled()) return mmap(addr,bytes,prot,flags,fd,offset);
    /* File mappings are not resident tensor ownership. Refuse them instead
     * of certifying an untracked fallback; callers may use their read path. */
    if (!bytes || !(flags&MAP_ANONYMOUS) || (flags&MAP_FIXED) || fd!=-1 || offset) {
        errno=ENOTSUP; return MAP_FAILED;
    }
    size_t extent;
    if (extent_for(bytes,&extent)) return MAP_FAILED;
    pthread_mutex_lock(&allocation_lock);
    if (evidence.fault || owner_pid!=getpid() ||
        evidence.requested_bytes>UINT64_MAX-bytes || evidence.locked_bytes>UINT64_MAX-extent) {
        (void)fault_locked(ENOMEM); pthread_mutex_unlock(&allocation_lock); return MAP_FAILED;
    }
    Allocation *entry=calloc(1,sizeof *entry);
    void *ptr=entry ? mmap(addr,bytes,prot,flags,fd,offset) : MAP_FAILED;
    int rc=!entry ? ENOMEM : ptr==MAP_FAILED ? errno : 0;
    if (!rc && mlock(ptr,extent)) {
        rc=errno;
        lmb_memlock_failure(extent,evidence.locked_bytes,rc);
    }
    if (rc) {
        if (ptr!=MAP_FAILED) (void)munmap(ptr,extent);
        free(entry); (void)fault_locked(rc); pthread_mutex_unlock(&allocation_lock); return MAP_FAILED;
    }
    entry->ptr=ptr; entry->requested=bytes; entry->extent=extent; entry->mapping=1;
    entry->next=allocations[bucket(ptr)]; allocations[bucket(ptr)]=entry;
    evidence.allocations++; evidence.requested_bytes+=bytes; evidence.locked_bytes+=extent;
    pthread_mutex_unlock(&allocation_lock); return ptr;
}
int lmb_engine_munmap(void *ptr, size_t bytes) {
    if (!enabled()) return munmap(ptr,bytes);
    pthread_mutex_lock(&allocation_lock);
    Allocation **at=lookup(ptr), *entry=*at;
    if (!entry) {
        int owned=overlap(ptr,bytes)!=NULL;
        pthread_mutex_unlock(&allocation_lock);
        if (owned) { errno=EINVAL; return -1; }
        return munmap(ptr,bytes);
    }
    if (!entry->mapping || (bytes!=entry->requested && bytes!=entry->extent)) {
        pthread_mutex_unlock(&allocation_lock); errno=EINVAL; return -1;
    }
    if (munmap(ptr,bytes)) { pthread_mutex_unlock(&allocation_lock); return -1; }
    *at=entry->next;
    evidence.allocations--; evidence.requested_bytes-=entry->requested; evidence.locked_bytes-=entry->extent;
    free(entry); pthread_mutex_unlock(&allocation_lock); return 0;
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
    if (!enabled()) return mlock(ptr,bytes);
    pthread_mutex_lock(&allocation_lock);
    int owned=owned_range(ptr,bytes);
    pthread_mutex_unlock(&allocation_lock);
    if (owned<0) { errno=EPERM; return -1; }
    return owned ? 0 : mlock(ptr,bytes);
}
int lmb_engine_munlock(const void *ptr, size_t bytes) {
    if (!enabled()) return munlock(ptr,bytes);
    pthread_mutex_lock(&allocation_lock);
    int owned=owned_range(ptr,bytes);
    pthread_mutex_unlock(&allocation_lock);
    if (owned<0) { errno=EPERM; return -1; }
    /* An upstream cache's optional pin is subordinate to resident ownership.
     * Release the one OS lock only when this allocation is actually freed. */
    return owned ? 0 : munlock(ptr,bytes);
}
int lmb_engine_mlockall(int flags) {
    if (!enabled()) return mlockall(flags);
    errno=ENOTSUP; return -1; /* no whole-process locking claim */
}
int lmb_engine_munlockall(void) {
    if (!enabled()) return munlockall();
    errno=EPERM; return -1;
}
int lmb_engine_madvise(void *ptr, size_t bytes, int advice) {
    if (!enabled()) return madvise(ptr,bytes,advice);
    pthread_mutex_lock(&allocation_lock);
    int owned=overlap(ptr,bytes)!=NULL;
    pthread_mutex_unlock(&allocation_lock);
    if (owned) {
        if (advice==MADV_NORMAL || advice==MADV_RANDOM || advice==MADV_SEQUENTIAL || advice==MADV_WILLNEED) return 0;
        errno=EPERM; return -1;
    }
    return madvise(ptr,bytes,advice);
}
