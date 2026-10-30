/* Explicit OS residency policy for a dedicated model process. Never changes
 * host swap settings or raises privileges. Observe != a pinning guarantee. */
#ifndef LMB_RESIDENT_GUARD_H
#define LMB_RESIDENT_GUARD_H
#include "lumabri_process_memory.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif
#if defined(LMB_ENGINE_MEMORY_OVERRIDE_H) || defined(LMB_ENGINE_MEMORY_LINKED)
#include "lumabri_engine_memory.h"
#ifdef _WIN32
#include "lumabri_engine_io_windows.h"
#else
#include <dlfcn.h>
#endif
#endif

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
    int enabled,locked,engine_locked;
    uint64_t process_budget;
    int (*retained_lock_status)(uint64_t *);
    _Atomic int fault;
} LmbResidentGuard;
enum { LMB_RESIDENT_OK=0,LMB_RESIDENT_PAGED=1,LMB_RESIDENT_COMPRESSED=2,LMB_RESIDENT_UNKNOWN=3,LMB_RESIDENT_LOCK_FAILED=4,LMB_RESIDENT_OVER_BUDGET=5 };

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
    case LMB_RESIDENT_LOCK_FAILED:return "engine allocation lock contract failed";
    case LMB_RESIDENT_OVER_BUDGET:return "process memory exceeds the approved reservation";
    default:return "resident observation current";
    }
}
static inline void lmb_resident_guard_report(LmbResidentGuard *g,const char *event,const LmbProcessMemory *m) {
#ifdef _WIN32
    _lock_file(stderr);
#else
    flockfile(stderr);
#endif
    fprintf(stderr,"[resident-memory] event=%s policy=%s fault=%d ",event,g->engine_locked?"engine-locked":g->locked?"locked":"observe",atomic_load(&g->fault));
    lmb_process_memory_json(stderr,m);fputc('\n',stderr);
#ifdef _WIN32
    _unlock_file(stderr);
#else
    funlockfile(stderr);
#endif
}
static inline int lmb_resident_guard_observe(LmbResidentGuard *g,const LmbProcessMemory *m) {
    if(!g->enabled)return 0;
    int reason=lmb_resident_memory_reason(m);
    if (g->engine_locked) {
        reason=LMB_RESIDENT_LOCK_FAILED;
#if defined(LMB_ENGINE_MEMORY_OVERRIDE_H) || defined(LMB_ENGINE_MEMORY_LINKED)
        LmbEngineMemory owned; lmb_engine_memory_snapshot(&owned);
        uint64_t retained=0;
        if ((m->known&LMB_PM_RSS) && owned.enabled && !owned.fault &&
            owned.allocations && owned.locked_bytes>=owned.requested_bytes &&
            g->retained_lock_status && !g->retained_lock_status(&retained)) reason=0;
#endif
        /* Compression of unrelated libc/code/stack pages is not evidence
         * against successfully locked engine payloads. It remains charged
         * by the process governor, and is never relabelled as zero. */
    }
    /* The engine's payload ledger does not include libc bookkeeping, thread
     * stacks or retained shim tables. Charge the native process as well. */
    if (!reason && g->process_budget && lmb_process_memory_charge(m)>g->process_budget)
        reason=LMB_RESIDENT_OVER_BUDGET;
    if(reason) {
        int expected=0;
        if(atomic_compare_exchange_strong(&g->fault,&expected,reason)) {
            if (reason==LMB_RESIDENT_OVER_BUDGET)
                fprintf(stderr,"[resident-memory] process_bytes=%llu approved_bytes=%llu\n",
                    (unsigned long long)lmb_process_memory_charge(m),(unsigned long long)g->process_budget);
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
    if(strcmp(mode,"observe") && strcmp(mode,"locked") && strcmp(mode,"engine-locked")) {
        fprintf(stderr,"[resident-memory] invalid LUMABRI_RESIDENCY_POLICY; use observe, locked or engine-locked\n");return -1;
    }
    g->enabled=1;
    if (!strcmp(mode,"engine-locked")) {
#if defined(LMB_ENGINE_MEMORY_OVERRIDE_H) || defined(LMB_ENGINE_MEMORY_LINKED)
        g->engine_locked=1;
#ifdef _WIN32
        g->retained_lock_status=lmb_engine_input_lock_status;
#else
        g->retained_lock_status=(int (*)(uint64_t *))dlsym(RTLD_DEFAULT,"lmb_weights_lock_status");
#endif
#else
        fprintf(stderr,"[resident-memory] engine allocation locking not compiled into this runtime; refusing READY\n");return -1;
#endif
    }
    if(!strcmp(mode,"locked")) {
#ifdef LMB_RESIDENT_ASAN
        fprintf(stderr,"[resident-memory] strict locking unavailable in AddressSanitizer builds; refusing READY\n");return -1;
#elif defined(_WIN32)
        fprintf(stderr,"[resident-memory] whole-process locking is unavailable on Windows; use engine-locked, refusing READY\n");return -1;
#else
        /* Lock current weights and future conversation/scratch allocations.
         * The OS must grant this, otherwise there is no READY and no fallback.
         * The dedicated engine releases locks on exit; no unrelated process
         * or machine-wide paging policy is changed. */
        if(mlockall(MCL_CURRENT|MCL_FUTURE)) {
            fprintf(stderr,"[resident-memory] cannot lock model process: %s; %s, refusing READY\n",strerror(errno),
                errno==ENOSYS || errno==ENOTSUP ? "OS does not implement whole-process locking" : "check the OS memlock allowance");return -1;
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
#if defined(LMB_ENGINE_MEMORY_OVERRIDE_H) || defined(LMB_ENGINE_MEMORY_LINKED)
    if (g->engine_locked) {
        LmbEngineMemory owned; lmb_engine_memory_snapshot(&owned);
        uint64_t retained=0; (void)g->retained_lock_status(&retained);
        fprintf(stderr,"[engine-memory] allocations=%llu requested_bytes=%llu locked_bytes=%llu retained_locked_bytes=%llu scope=owned-cpu-allocations\n",
            (unsigned long long)owned.allocations,(unsigned long long)owned.requested_bytes,
            (unsigned long long)owned.locked_bytes,(unsigned long long)retained);
    }
#endif
    lmb_resident_guard_report(g,"ready",&m);
    return 0;
}
#endif
