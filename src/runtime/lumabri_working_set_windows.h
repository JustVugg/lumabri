/* Process-local VirtualLock quota shared by the native Edge and Segment
 * executables. Actual owned locks and residency guards remain authoritative. */
#ifndef LMB_WORKING_SET_WINDOWS_H
#define LMB_WORKING_SET_WINDOWS_H
#ifdef _WIN32
#include "lumabri_machine.h"
#include "lumabri_memory_budget.h"
static inline int lmb_windows_working_set(uint64_t requested,const char *role_reserve) {
    uint64_t allowed=0;
    const char *reserve_name=getenv(role_reserve) ? role_reserve : "LUMABRI_RAM_RESERVE_MB";
    uint64_t reserve=(uint64_t)lmb_env_int(reserve_name,
        (int)lmb_machine_default_reserve_mb(lmb_machine_total_ram()),256,262144)<<20;
    /* Never raise a system-wide limit. Setting a quota is not a guarantee:
     * each engine allocation must still acquire and retain its page locks. */
    if (!requested || requested>SIZE_MAX ||
        lmb_process_budget(requested,lmb_machine_available_ram(),reserve,&allowed) || allowed<requested ||
        !SetProcessWorkingSetSize(GetCurrentProcess(),(SIZE_T)requested,(SIZE_T)requested)) {
        fprintf(stderr,"[resident-memory] Windows cannot admit the explicit %llu-byte process working set; "
            "check the approved memory limit and available RAM (OS error %lu)\n",
            (unsigned long long)requested,(unsigned long)GetLastError());
        return -1;
    }
    return 0;
}
#endif
#endif
