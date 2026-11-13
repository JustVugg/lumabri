#define _POSIX_C_SOURCE 200809L
#include "src/runtime/lumabri_memlock.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    LmbMemlockLimit limit={.known=1,.bytes=64u<<20};
    assert(!lmb_memlock_insufficient(&limit,64u<<20));
    assert(lmb_memlock_insufficient(&limit,(64u<<20)+1u));
    assert(!lmb_memlock_insufficient(&limit,0));
    assert(!lmb_memlock_insufficient(&limit,UINT64_MAX));
    limit.known=0; assert(!lmb_memlock_insufficient(&limit,UINT64_MAX-1));
    limit.known=1; limit.exempt=1;
    assert(!lmb_memlock_insufficient(&limit,UINT64_MAX-1));
    limit.exempt=0; limit.unlimited=1;
    assert(!lmb_memlock_insufficient(&limit,UINT64_MAX-1));
    /* The automatic household choice: lock exactly when a known allowance
     * covers the whole reservation; unknown, unlimited and exempt allowances
     * are left to native lock enforcement, as engine-locked always was. */
    limit=(LmbMemlockLimit){.known=1,.bytes=64u<<20};
    assert(lmb_memlock_auto_locks(&limit,64u<<20));
    assert(!lmb_memlock_auto_locks(&limit,(64u<<20)+1u));
    assert(!lmb_memlock_auto_locks(&limit,700u<<20));
    limit.bytes=0; assert(!lmb_memlock_auto_locks(&limit,4096));
    limit.unlimited=1; assert(lmb_memlock_auto_locks(&limit,UINT64_MAX-1));
    limit=(LmbMemlockLimit){.known=1,.exempt=1}; assert(lmb_memlock_auto_locks(&limit,UINT64_MAX-1));
    limit=(LmbMemlockLimit){0}; assert(lmb_memlock_auto_locks(&limit,UINT64_MAX-1));
    assert(lmb_residency_unlocked(LMB_RESIDENCY_AUTO_UNLOCKED)==1 && lmb_residency_unlocked(LMB_RESIDENCY_OBSERVE)==1);
    assert(!lmb_residency_unlocked(LMB_RESIDENCY_AUTO_LOCKED) && !lmb_residency_unlocked(LMB_RESIDENCY_ENGINE_LOCKED) &&
           !lmb_residency_unlocked(LMB_RESIDENCY_PROCESS_LOCKED) && lmb_residency_unlocked(LMB_RESIDENCY_UNREPORTED)<0);
    assert(!strcmp(lmb_residency_name(LMB_RESIDENCY_AUTO_UNLOCKED),"automatic-not-locked") &&
           !strcmp(lmb_residency_name(99),"unreported"));
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        struct rlimit zero={0,0}, after;
        assert(!setrlimit(RLIMIT_MEMLOCK,&zero));
        assert(!setenv("LUMABRI_RESIDENCY_POLICY","observe",1));
        assert(!lmb_memlock_preflight(4096,"test"));
        assert(!setenv("LUMABRI_RESIDENCY_POLICY","engine-locked",1));
        limit=lmb_memlock_limit();
        assert((lmb_memlock_preflight(4096,"test")<0)==
            (limit.known && !limit.exempt && !limit.unlimited));
        /* "auto" is a launcher word. An engine that saw it would not treat
         * it as a request to lock, and the guard rejects it as invalid. */
        assert(!setenv("LUMABRI_RESIDENCY_POLICY","auto",1));
        assert(!lmb_memlock_preflight(4096,"test"));
        assert(!getrlimit(RLIMIT_MEMLOCK,&after));
        assert(!after.rlim_cur && !after.rlim_max); /* never raises allowance */
        _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    puts("MEMLOCK PREFLIGHT: PASS (known/unknown, exact boundary, privilege, automatic choice, no policy mutation)");
    return 0;
}
