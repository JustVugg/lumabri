#define _GNU_SOURCE
#include "src/runtime/lumabri_engine_memory.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static LmbEngineMemory snapshot(void) {
    LmbEngineMemory value; lmb_engine_memory_snapshot(&value); return value;
}
static void *parallel(void *unused) {
    (void)unused;
    for (int i=0;i<100;i++) {
        char *p=lmb_engine_calloc(3,19); assert(p && !p[0] && !p[56]);
        p[0]=42; p=lmb_engine_realloc(p,129); assert(p && p[0]==42);
        lmb_engine_free(p);
    }
    return NULL;
}
static void normal(void) {
    setenv("LUMABRI_RESIDENCY_POLICY","observe",1);
    char *p=lmb_engine_strdup("ordinary"); assert(p && !strcmp(p,"ordinary"));
    p=lmb_engine_realloc(p,100); assert(p); lmb_engine_free(p);
    assert(!snapshot().enabled && !snapshot().allocations);
}
static void denial(void) {
    setenv("LUMABRI_RESIDENCY_POLICY","engine-locked",1);
    struct rlimit limit={0,0}; assert(!setrlimit(RLIMIT_MEMLOCK,&limit));
    void *p=lmb_engine_malloc(4096);
    assert(!p && snapshot().enabled && snapshot().fault && !snapshot().locked_bytes);
    assert(!lmb_engine_malloc(1)); /* a failed lock is never silently retried */
}
static void ownership(void) {
    setenv("LUMABRI_RESIDENCY_POLICY","engine-locked",1);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char *p=lmb_engine_malloc(17);
    if (!p) {
        assert(snapshot().fault && !snapshot().locked_bytes);
        puts("ENGINE LOCK: unavailable under this process's OS allowance/instrumentation");
        return;
    }
    assert(!((uintptr_t)p%page)); memset(p,0x5a,17);
    LmbEngineMemory s=snapshot();
    assert(s.enabled && !s.fault && s.allocations==1 && s.requested_bytes==17 && s.locked_bytes==page);
    pid_t forked=fork(); assert(forked>=0);
    if (!forked) {
        assert(snapshot().fault==ESTALE && !lmb_engine_malloc(1));
        _exit(0);
    }
    int status; assert(waitpid(forked,&status,0)==forked && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!snapshot().fault); /* child's refusal did not alter parent's lock */
    p=lmb_engine_realloc(p,page+1); assert(p && p[0]==0x5a && p[16]==0x5a);
    s=snapshot(); assert(s.allocations==1 && s.requested_bytes==page+1 && s.locked_bytes==page*2);
    void *unchanged=(void *)(uintptr_t)1;
    assert(lmb_engine_posix_memalign(&unchanged,3,10)==EINVAL && unchanged==(void *)(uintptr_t)1);
    void *aligned=NULL;
    assert(!lmb_engine_posix_memalign(&aligned,page*4,23) && !((uintptr_t)aligned%(page*4)));
    lmb_engine_free(aligned);
    aligned=lmb_engine_aligned_alloc(1,7); assert(aligned && !((uintptr_t)aligned%page));
    lmb_engine_free(aligned);
    assert(!lmb_engine_calloc(SIZE_MAX,2));
    assert(!lmb_engine_realloc(p,SIZE_MAX)); assert(p[0]==0x5a); /* original survives overflow */
    char *text=lmb_engine_strndup("abcdef",3); assert(text && !strcmp(text,"abc"));
    lmb_engine_free(text); lmb_engine_free(p);
    assert(!snapshot().allocations && !snapshot().locked_bytes && !snapshot().fault);
    /* libc ownership can cross the boundary without corrupting the registry. */
    char *foreign=strdup("libc"); assert(foreign); lmb_engine_free(foreign);
    void *mapped=lmb_engine_mmap(NULL,page+1,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapped!=MAP_FAILED && snapshot().locked_bytes==page*2);
    memset(mapped,0x42,page+1);
    assert(lmb_engine_munmap((char *)mapped+page,page)==-1);
    assert(!lmb_engine_mlock(mapped,page) && !lmb_engine_munlock(mapped,page));
    assert(lmb_engine_madvise(mapped,page,MADV_DONTNEED)==-1);
    assert(lmb_engine_munlockall()==-1 && snapshot().locked_bytes==page*2);
    assert(lmb_engine_munmap(mapped,page)==-1); /* partial removal cannot leave stale ownership */
    assert(!lmb_engine_munmap(mapped,page+1) && !snapshot().allocations);
    assert(lmb_engine_mmap(NULL,page,PROT_READ,MAP_PRIVATE,0,0)==MAP_FAILED);
    pthread_t threads[4];
    for (unsigned i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,parallel,NULL));
    for (unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    s=snapshot(); assert(!s.fault && !s.allocations && !s.locked_bytes);
    puts("ENGINE LOCK: PASS (native page locks, aligned ownership, realloc, mappings, concurrent release)");
}
static void child(void (*test)(void)) {
    fflush(NULL);
    pid_t pid=fork(); assert(pid>=0);
    if (!pid) { test(); fflush(NULL); _exit(0); }
    int status; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main(void) {
    child(normal); child(ownership); child(denial);
    puts("ENGINE MEMORY: PASS (ordinary ABI, explicit native availability, denied-lock fault, ownership)");
    return 0;
}
