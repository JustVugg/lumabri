/* Explicit OS residency policy for a dedicated model process. Never changes
 * host swap settings or raises privileges. Observe != a pinning guarantee. */
#ifndef LMB_RESIDENT_GUARD_H
#define LMB_RESIDENT_GUARD_H
#include "lumabri_process_memory.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/mman.h>

/* AddressSanitizer intercepts locking calls as successful no-ops. A sanitizer
 * binary must not advertise that its pages were pinned by the OS. */
#if defined(__SANITIZE_ADDRESS__)
#define LMB_RESIDENT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LMB_RESIDENT_ASAN 1
#endif
#endif

typedef struct {
    int enabled,locked;
    _Atomic int fault;
} LmbResidentGuard;
enum { LMB_RESIDENT_OK=0,LMB_RESIDENT_PAGED=1,LMB_RESIDENT_COMPRESSED=2,LMB_RESIDENT_UNKNOWN=3 };

static inline int lmb_resident_memory_reason(const LmbProcessMemory *m) {
    if(!(m->known&LMB_PM_RSS))return LMB_RESIDENT_UNKNOWN;
    if((m->known&LMB_PM_SWAP) && m->swap)return LMB_RESIDENT_PAGED;
    if((m->known&LMB_PM_COMPRESSED) && m->compressed)return LMB_RESIDENT_COMPRESSED;
    if(!(m->known&(LMB_PM_SWAP|LMB_PM_COMPRESSED)))return LMB_RESIDENT_UNKNOWN;
    return LMB_RESIDENT_OK;
}
static inline const char *lmb_resident_memory_reason_text(int reason) {
    switch(reason) {
    case LMB_RESIDENT_PAGED:return "process pages moved to swap; resident preparation must be renewed";
    case LMB_RESIDENT_COMPRESSED:return "process pages compressed; physical residency no longer verified";
    case LMB_RESIDENT_UNKNOWN:return "process memory evidence unavailable";
    default:return "resident observation current";
    }
}
static inline void lmb_resident_guard_report(LmbResidentGuard *g,const char *event,const LmbProcessMemory *m) {
    flockfile(stderr);
    fprintf(stderr,"[resident-memory] event=%s policy=%s fault=%d ",event,g->locked?"locked":"observe",atomic_load(&g->fault));
    lmb_process_memory_json(stderr,m);fputc('\n',stderr);funlockfile(stderr);
}
static inline int lmb_resident_guard_observe(LmbResidentGuard *g,const LmbProcessMemory *m) {
    if(!g->enabled)return 0;
    int reason=lmb_resident_memory_reason(m);
    if(reason) {
        int expected=0;
        if(atomic_compare_exchange_strong(&g->fault,&expected,reason)) {
            fprintf(stderr,"[resident-memory] %s; refusing inference, weights are not silently reloaded\n",lmb_resident_memory_reason_text(reason));
            lmb_resident_guard_report(g,"lost",m);
        }
    }
    return atomic_load(&g->fault)?-1:0;
}
static inline int lmb_resident_guard_check(LmbResidentGuard *g) {
    if(!g->enabled)return 0;
    if(atomic_load(&g->fault))return -1;
    LmbProcessMemory m={0};(void)lmb_process_memory_probe(&m,0);
    return lmb_resident_guard_observe(g,&m);
}
static inline int lmb_resident_guard_begin(LmbResidentGuard *g) {
    const char *mode=getenv("LUMABRI_RESIDENCY_POLICY");
    if(!mode || !*mode)mode="observe";
    if(strcmp(mode,"observe") && strcmp(mode,"locked")) {
        fprintf(stderr,"[resident-memory] invalid LUMABRI_RESIDENCY_POLICY; use observe or locked\n");return -1;
    }
    g->enabled=1;
    if(!strcmp(mode,"locked")) {
#ifdef LMB_RESIDENT_ASAN
        fprintf(stderr,"[resident-memory] strict locking unavailable in AddressSanitizer builds; refusing READY\n");return -1;
#else
        /* Lock current weights and future conversation/scratch allocations.
         * The OS must grant this, otherwise there is no READY and no fallback.
         * The dedicated engine releases locks on exit; no unrelated process
         * or machine-wide paging policy is changed. */
        if(mlockall(MCL_CURRENT|MCL_FUTURE)) {
            fprintf(stderr,"[resident-memory] cannot lock model process: %s; check the OS memlock allowance, refusing READY\n",strerror(errno));return -1;
        }
        g->locked=1;
#endif
    }
    LmbProcessMemory m={0};(void)lmb_process_memory_probe(&m,1);
    if(g->locked && (m.known&LMB_PM_LOCKED) && !m.locked) {
        g->locked=0;
        fprintf(stderr,"[resident-memory] lock call succeeded but the OS reports zero locked bytes; refusing READY\n");return -1;
    }
    if(lmb_resident_guard_observe(g,&m))return -1;
    lmb_resident_guard_report(g,"ready",&m);
    return 0;
}
#endif
