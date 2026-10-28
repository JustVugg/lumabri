#include "src/runtime/lumabri_engine_memory.h"
#include "src/runtime/lumabri_engine_memory_os.h"
#include <psapi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
void *lmb_test_native_malloc(size_t size);
char *lmb_test_native_strdup(const char *text);
void lmb_test_native_free(void *ptr);
void *lmb_test_native_aligned(size_t size);
void lmb_test_native_aligned_free(void *ptr);

static LmbEngineMemory snapshot(void) {
    LmbEngineMemory value; lmb_engine_memory_snapshot(&value); return value;
}
static void locked(const void *ptr) {
    PSAPI_WORKING_SET_EX_INFORMATION page={0};
    page.VirtualAddress=(void *)ptr;
    assert(QueryWorkingSetEx(GetCurrentProcess(),&page,sizeof page));
    assert(page.VirtualAttributes.Valid && page.VirtualAttributes.Locked);
}
static DWORD WINAPI parallel(LPVOID unused) {
    (void)unused;
    for (unsigned i=0;i<100;i++) {
        char *p=lmb_engine_calloc(3,19); assert(p && !p[0] && !p[56]);
        p[0]=42; p=lmb_engine_realloc(p,129); assert(p && p[0]==42);
        locked(p); lmb_engine_free(p);
    }
    return 0;
}
static void ordinary(void) {
    assert(!_putenv_s("LUMABRI_RESIDENCY_POLICY","observe"));
    char *plain=lmb_engine_strdup("ordinary"); assert(plain);
    plain=lmb_engine_realloc(plain,100); assert(plain && !strcmp(plain,"ordinary"));
    lmb_engine_free(plain);
    void *aligned=NULL;
    assert(!lmb_engine_posix_memalign(&aligned,4096,17) && !((uintptr_t)aligned%4096));
    memset(aligned,42,17);
    aligned=lmb_engine_realloc(aligned,32); assert(aligned && ((char *)aligned)[0]==42);
    lmb_engine_free(aligned);
    aligned=lmb_engine_win_aligned_malloc(23,64); assert(aligned && !((uintptr_t)aligned%64));
    lmb_engine_win_aligned_free(aligned);
    void *foreign=_aligned_malloc(23,64); assert(foreign); lmb_engine_win_aligned_free(foreign);
    assert(!snapshot().enabled && !snapshot().allocations && !snapshot().locked_bytes);
}
static void denied(void) {
    assert(!_putenv_s("LUMABRI_RESIDENCY_POLICY","engine-locked"));
    /* Restrict only this disposable process. The allocator must neither
     * enlarge its working set nor accept unlocked memory after refusal. */
    assert(SetProcessWorkingSetSize(GetCurrentProcess(),64u*1024u,128u*1024u));
    void *ptr=lmb_engine_malloc(4u*1024u*1024u);
    assert(!ptr && snapshot().fault && !snapshot().allocations && !snapshot().locked_bytes);
    assert(!lmb_engine_malloc(1));
}
static void ownership(void) {
    assert(!_putenv_s("LUMABRI_RESIDENCY_POLICY","engine-locked"));
    /* Explicit allowance belongs to the test's process, never the allocator. */
    assert(SetProcessWorkingSetSize(GetCurrentProcess(),4u*1024u*1024u,16u*1024u*1024u));
    size_t page=lmb_memory_pagesize();
    void *hook=lmb_test_native_malloc(page*2); assert(hook); locked(hook);
    assert(snapshot().allocations==1); lmb_test_native_free(hook);
    char *text=lmb_test_native_strdup("engine boundary"); assert(text && !strcmp(text,"engine boundary"));
    locked(text); lmb_test_native_free(text);
    hook=lmb_test_native_aligned(37); assert(hook && !((uintptr_t)hook%64));
    locked(hook); lmb_test_native_aligned_free(hook);
    assert(!snapshot().allocations && !snapshot().locked_bytes);
    enum { COUNT=2048 };
    void *objects[COUNT]; uintptr_t pages[COUNT*2]; size_t unique=0, requested=0;
    for (unsigned i=0;i<COUNT;i++) {
        size_t size=8+i%48;
        objects[i]=lmb_engine_malloc(size); assert(objects[i]);
        memset(objects[i],0x5a,size); requested+=size;
        uintptr_t first=(uintptr_t)objects[i]&~(uintptr_t)(page-1);
        uintptr_t last=((uintptr_t)objects[i]+size-1)&~(uintptr_t)(page-1);
        for(uintptr_t p=first;;p+=page) {
            size_t j=0; while(j<unique && pages[j]!=p) j++;
            if(j==unique) pages[unique++]=p;
            locked((void *)p); if(p==last) break;
        }
    }
    LmbEngineMemory s=snapshot();
    assert(!s.fault && s.allocations==COUNT && s.requested_bytes==requested);
    assert(s.locked_bytes==unique*page && unique<COUNT/8);
    for(unsigned i=0;i<COUNT;i+=2) lmb_engine_free(objects[i]);
    assert(EmptyWorkingSet(GetCurrentProcess()));
    for(unsigned i=1;i<COUNT;i+=2) {
        locked(objects[i]); assert(((unsigned char *)objects[i])[7+i%48]==0x5a);
        lmb_engine_free(objects[i]);
    }
    assert(!snapshot().allocations && !snapshot().locked_bytes && !snapshot().fault);
    char *p=lmb_engine_malloc(page+1); assert(p); memset(p,42,page+1);
    locked(p); locked(p+page);
    assert(snapshot().locked_bytes==2*page);
    assert(!lmb_engine_munlock(p,page)); locked(p);
    assert(lmb_engine_madvise(p,page,MADV_DONTNEED)==-1);
    assert(lmb_engine_munlockall()==-1); locked(p);
    assert(!lmb_engine_realloc(p,SIZE_MAX) && p[page]==42);
    p=lmb_engine_realloc(p,23); assert(p && p[0]==42); lmb_engine_free(p);
    void *aligned=lmb_engine_win_aligned_malloc(31,page*4);
    assert(aligned && !((uintptr_t)aligned%(page*4))); locked(aligned);
    lmb_engine_win_aligned_free(aligned);
    void *mapped=lmb_engine_mmap(NULL,page+1,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mapped!=MAP_FAILED); locked(mapped); locked((char *)mapped+page);
    assert(lmb_engine_munmap(mapped,page)==-1);
    assert(lmb_engine_munmap((char *)mapped+page,page)==-1);
    assert(!lmb_engine_munmap(mapped,page+1));
    assert(lmb_engine_mmap(NULL,page,PROT_READ,MAP_PRIVATE,0,0)==MAP_FAILED);
    HANDLE threads[4];
    for(unsigned i=0;i<4;i++) { threads[i]=CreateThread(NULL,0,parallel,NULL,0,NULL); assert(threads[i]); }
    assert(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;i++) CloseHandle(threads[i]);
    assert(!snapshot().allocations && !snapshot().locked_bytes && !snapshot().fault);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"ordinary")) ordinary();
    else if(!strcmp(argv[1],"denied")) denied();
    else if(!strcmp(argv[1],"ownership")) ownership();
    else return 2;
    printf("WINDOWS ENGINE MEMORY: PASS (%s)\n",argv[1]);
    return 0;
}
