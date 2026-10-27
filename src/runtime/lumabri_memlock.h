/* Diagnose strict CPU residency without raising OS limits or probing by
 * allocating the model twice. An admitted estimate is NOT proof of a lock:
 * the allocator and retained-table contracts remain authoritative. */
#ifndef LMB_MEMLOCK_H
#define LMB_MEMLOCK_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>

#define LMB_MEMLOCK_EXIT 78

typedef struct {
    uint64_t bytes;
    int known, unlimited, exempt;
} LmbMemlockLimit;

static inline LmbMemlockLimit lmb_memlock_limit(void) {
    LmbMemlockLimit out={0};
#if defined(__linux__) && defined(RLIMIT_MEMLOCK)
    /* CAP_IPC_LOCK exempts Linux processes from RLIMIT_MEMLOCK. UID alone
     * is not evidence of that capability (including in containers). If its
     * effective state cannot be read, defer to the actual mlock call. */
    FILE *f=fopen("/proc/self/status","r");
    if (!f) return out;
    char line[256]; unsigned long long caps=0; int found=0;
    while (fgets(line,sizeof line,f)) {
        if (!strncmp(line,"CapEff:",7)) {
            found=sscanf(line+7,"%llx",&caps)==1; break;
        }
    }
    fclose(f);
    struct rlimit limit;
    if (!found || getrlimit(RLIMIT_MEMLOCK,&limit)) return out;
    out.known=1;
    out.exempt=!!(caps&(1ULL<<14)); /* CAP_IPC_LOCK */
    out.unlimited=limit.rlim_cur==RLIM_INFINITY;
    out.bytes=out.unlimited ? UINT64_MAX : (uint64_t)limit.rlim_cur;
#endif
    /* Other kernels have different wiring limits. Do not assume Linux's
     * admission semantics; native allocation failures still explain mlock. */
    return out;
}

static inline int lmb_memlock_insufficient(const LmbMemlockLimit *limit,
                                           uint64_t weight_bytes) {
    return weight_bytes && weight_bytes!=UINT64_MAX && limit->known &&
        !limit->unlimited && !limit->exempt && weight_bytes>limit->bytes;
}

static inline int lmb_memlock_preflight(uint64_t weight_bytes, const char *role) {
    const char *mode=getenv("LUMABRI_RESIDENCY_POLICY");
    if (!mode || (strcmp(mode,"engine-locked") && strcmp(mode,"locked"))) return 0;
    LmbMemlockLimit limit=lmb_memlock_limit();
    if (!lmb_memlock_insufficient(&limit,weight_bytes)) return 0;
    fprintf(stderr,"[resident-memory] %s requires at least %llu bytes; "
        "OS locked-memory allowance (RLIMIT_MEMLOCK) is %llu bytes. "
        "Refusing before loading weights. Ask the machine owner to configure "
        "the Lumabri service's memlock allowance; no limits were changed.\n",
        role,(unsigned long long)weight_bytes,(unsigned long long)limit.bytes);
    errno=ENOMEM;
    return -1;
}

static inline int lmb_memlock_startup(void) {
    /* The config/manifest parser is namespaced too. Explain a zero/sub-page
     * allowance before even its first allocation can fail. */
    long page=sysconf(_SC_PAGESIZE);
    return page>0 ? lmb_memlock_preflight((uint64_t)page,"CPU runtime's first locked page") : 0;
}

static inline void lmb_memlock_failure(size_t bytes, uint64_t owned, int error) {
    LmbMemlockLimit limit=lmb_memlock_limit();
    fprintf(stderr,"[resident-memory] mlock failed: %s; requested=%llu bytes, "
        "already_owned_locked=%llu bytes",strerror(error),
        (unsigned long long)bytes,(unsigned long long)owned);
    if (limit.known && !limit.unlimited && !limit.exempt)
        fprintf(stderr,", RLIMIT_MEMLOCK=%llu bytes",(unsigned long long)limit.bytes);
    fprintf(stderr,". Check the OS locked-memory allowance and available RAM; "
        "no fallback to unlocked weights.\n");
}
#endif
