/* Start the shared manager as an independent, same-user Windows process.
 * Only this manager (which owns no engines) may be detached. Engine children
 * retain their kill-on-close ownership job for their whole lifetime. */
#ifndef LUMABRI_MANAGER_WINDOWS_H
#define LUMABRI_MANAGER_WINDOWS_H
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_ready.h"

static int home_service_manager_entry_windows(void) {
    /* This is a re-exec entry, not a second way to bypass startup checks. */
    char handle[32];
    DWORD n = GetEnvironmentVariableA("LUMABRI_READY_HANDLE", handle, sizeof handle);
    if (!n || n >= sizeof handle) return 2;
    HomeService manager;
    if (home_service_open(&manager, "manager")) return errno == EBUSY ? 75 : 1;
    snprintf(manager.snapshot.name, sizeof manager.snapshot.name, "Household manager");
    if (home_service_save(&manager) || lmb_ready_notify()) {
        home_service_close(&manager); return 1;
    }
    return home_service_manager_run(&manager);
}

static int home_service_manager_start_windows(void) {
    wchar_t executable[2048]; char binary[6144], log_path[1200];
    DWORD n = GetModuleFileNameW(NULL, executable, 2048);
    if (!n || n >= 2048 || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            executable, -1, binary, sizeof binary, NULL, NULL) ||
        home_service_path("manager", "log", log_path, sizeof log_path)) return -1;
    HANDLE log = lmb_private_open(log_path, 1, 1, NULL);
    if (log == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER end = {0};
    if (!SetFilePointerEx(log, end, NULL, FILE_END)) { CloseHandle(log); return -1; }
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ready) { CloseHandle(log); return -1; }
    const char *args[] = {"--internal-manager"};
    LmbChildHandle handoff = {"LUMABRI_READY_HANDLE", ready};
    LmbProcessLaunch launch = {.binary = binary, .args = args, .nargs = 1,
        .output = log, .error = log, .handles = &handoff, .nhandles = 1};
    LmbOwnedProcess child = {0};
    int rc = lmb_owned_process_start(&child, &launch);
    CloseHandle(log);
    if (rc) { CloseHandle(ready); return -1; }
    HANDLE waits[] = {ready, child.process};
    DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 5000);
    CloseHandle(ready);
    HomeServiceSnapshot snapshot;
    int observed = 0;
    if (wait == WAIT_OBJECT_0) {
        /* Concurrent starters can briefly occupy the single pipe instance.
         * READY alone is not enough, but a busy endpoint is not failure. */
        ULONGLONG until = GetTickCount64() + 2000;
        do {
            if (!home_service_query("manager", HOME_SVC_STATUS, NULL, &snapshot)) {
                observed = snapshot.pid == child.pid && snapshot.state == HOME_SVC_RUNNING;
                break;
            }
            home_service_wait_ms(20);
        } while (GetTickCount64() < until && WaitForSingleObject(child.process, 0) == WAIT_TIMEOUT);
    }
    if (observed) {
        /* READY and the authenticated endpoint must agree on this actual
         * child. Until now a crashed starter kills its incomplete manager.
         * After handoff, closing the terminal must leave it alive. */
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        if (QueryInformationJobObject(child.job, JobObjectExtendedLimitInformation,
                &limits, sizeof limits, NULL)) {
            limits.BasicLimitInformation.LimitFlags &= ~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(child.job, JobObjectExtendedLimitInformation,
                    &limits, sizeof limits)) {
                CloseHandle(child.process); CloseHandle(child.job);
                return 0;
            }
        }
    }
    DWORD code = 1;
    int raced = wait == WAIT_OBJECT_0 + 1 && GetExitCodeProcess(child.process, &code) && code == 75;
    /* The only process eligible for cancellation is the handle just created;
     * never a keeper found through a saved PID or another starter's record. */
    (void)lmb_owned_process_stop(&child, 0, 5000);
    if (lmb_owned_process_close(&child)) {
        CloseHandle(child.process); CloseHandle(child.job);
    }
    if (raced) {
        for (unsigned i = 0; i < 40; i++) {
            if (!home_service_query("manager", HOME_SVC_STATUS, NULL, &snapshot)) return 0;
            home_service_wait_ms(50);
        }
    }
    return -1;
}
#endif
