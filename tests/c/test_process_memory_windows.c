/* Native Win32 evidence: a cross-compile alone does not satisfy this gate.
 * Only this process's private pages and working set are changed. */
#include "src/runtime/lumabri_process_memory.h"
#include <assert.h>
#include <stdlib.h>

static LmbProcessMemory probe(void) {
    LmbProcessMemory m={0};
    assert(!lmb_process_memory_probe(&m,1));
    assert(!strcmp(m.source,"windows_process_memory"));
    assert((m.known&(LMB_PM_RSS|LMB_PM_PRIVATE_COMMIT))==
           (LMB_PM_RSS|LMB_PM_PRIVATE_COMMIT));
    assert(!(m.known&(LMB_PM_SWAP|LMB_PM_LOCKED|LMB_PM_MAJOR_FAULTS|
                      LMB_PM_COMPRESSED|LMB_PM_FOOTPRINT)));
    assert(m.rss && m.private_commit);
    assert(lmb_process_memory_charge(&m)>=m.rss);
    assert(lmb_process_memory_charge(&m)>=m.private_commit);
    return m;
}

static void locked(void *address) {
    PSAPI_WORKING_SET_EX_INFORMATION page={0};
    page.VirtualAddress=address;
    assert(QueryWorkingSetEx(GetCurrentProcess(),&page,sizeof page));
    assert(page.VirtualAttributes.Valid && page.VirtualAttributes.Locked);
}

int main(void) {
    SYSTEM_INFO system; GetSystemInfo(&system);
    const SIZE_T bytes=32u*1024u*1024u;
    LmbProcessMemory before=probe();
    unsigned char *memory=VirtualAlloc(NULL,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    assert(memory);
    LmbProcessMemory committed=probe();
    assert(committed.private_commit>=before.private_commit+bytes);
    /* Committing is not proof of residence; deliberately touch every page. */
    for(SIZE_T i=0;i<bytes;i+=system.dwPageSize) memory[i]=0x5a;
    assert(VirtualLock(memory,system.dwPageSize));
    locked(memory);
    LmbProcessMemory resident=probe();
    assert(resident.rss>=bytes);
    /* Working-set trimming must not create capacity from live private
     * allocations. The locked page remains resident without raising limits. */
    assert(EmptyWorkingSet(GetCurrentProcess()));
    locked(memory);
    LmbProcessMemory trimmed=probe();
    assert(trimmed.rss<resident.rss);
    assert(trimmed.private_commit>=before.private_commit+bytes);
    assert(lmb_process_memory_charge(&trimmed)>=before.private_commit+bytes);
    assert(memory[0]==0x5a);
    assert(VirtualUnlock(memory,system.dwPageSize));
    assert(VirtualFree(memory,0,MEM_RELEASE));
    LmbProcessMemory after=probe();
    assert(after.private_commit<trimmed.private_commit);
    lmb_process_memory_json(stdout,&trimmed); putchar('\n');
    puts("WINDOWS PROCESS MEMORY: PASS (native commit, resident pages, trim, release; unknown swap remains null)");
    return 0;
}
