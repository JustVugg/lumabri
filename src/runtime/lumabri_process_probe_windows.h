/* Bounded executable probes share native engine ownership. No shell or
 * PID-based signalling; every process in this temporary job is owned here. */
#ifndef LUMABRI_PROCESS_PROBE_WINDOWS_H
#define LUMABRI_PROCESS_PROBE_WINDOWS_H
#ifdef _WIN32
#include "lumabri_owned_process_windows.h"

static int lmb_windows_probe_capture(const char *binary, const char *const *args,
        size_t nargs, char *reply, size_t cap, size_t *length, DWORD *exit_code,
        unsigned timeout_ms) {
    if (!binary || !*binary || !reply || cap < 2 || cap > UINT32_MAX || !length ||
        !exit_code || !timeout_ms || timeout_ms > 60000 || nargs > 64 || (nargs && !args)) {
        errno = EINVAL; return -1;
    }
    *length = 0; *exit_code = MAXDWORD; reply[0] = 0; errno = EIO;
    HANDLE input = NULL, output = NULL;
    LmbOwnedProcess child = {0}; int rc = -1;
    if (!CreatePipe(&input, &output, NULL, 0)) return -1;
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error == INVALID_HANDLE_VALUE) error = NULL;
    LmbProcessLaunch launch = {.binary=binary, .args=args, .nargs=nargs, .output=output, .error=error};
    if (lmb_owned_process_start(&child, &launch)) goto done;
    CloseHandle(output); output = NULL;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (GetTickCount64() >= deadline) { errno = ETIMEDOUT; goto done; }
        DWORD available = 0, got = 0;
        if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL) && GetLastError() != ERROR_BROKEN_PIPE) goto done;
        if (available) {
            if (available >= cap - *length) { errno = EMSGSIZE; goto done; }
            if (!ReadFile(input, reply + *length, available, &got, NULL) || !got) goto done;
            *length += got; reply[*length] = 0;
        }
        DWORD state = WaitForSingleObject(child.process, 0);
        if (state == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) goto done;
            } else if (available) continue;
            if (!GetExitCodeProcess(child.process, exit_code)) goto done;
            rc = 0; break;
        }
        if (state == WAIT_FAILED) goto done;
        Sleep(5);
    }
done:
    if (child.process) {
        int saved = errno;
        if (lmb_owned_process_stop(&child, 0, 5000)) {
            /* No resident allocation exists for a capability probe. Closing
             * its sole job handle also requests kernel cleanup, but never
             * report a successful probe without confirmed descendant exit. */
            rc = -1; CloseHandle(child.process); CloseHandle(child.job);
        } else (void)lmb_owned_process_close(&child);
        errno = saved;
    }
    if (input) CloseHandle(input);
    if (output) CloseHandle(output);
    return rc;
}
#endif
#endif
