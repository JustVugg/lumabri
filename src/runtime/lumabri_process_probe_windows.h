/* Bounded native executable probes. No shell, PID-based signalling, or
 * persistent services: every process in this temporary job is owned here. */
#ifndef LUMABRI_PROCESS_PROBE_WINDOWS_H
#define LUMABRI_PROCESS_PROBE_WINDOWS_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int lmb_probe_arg(wchar_t *command, size_t cap, size_t *used, const wchar_t *arg) {
    size_t n = *used;
#define LMB_PROBE_CHAR(c) do { if (n + 1 >= cap) return -1; command[n++] = (c); } while (0)
    if (n) LMB_PROBE_CHAR(L' ');
    LMB_PROBE_CHAR(L'"');
    while (*arg) {
        size_t slashes = 0;
        while (*arg == L'\\') { slashes++; arg++; }
        size_t escaped = *arg == L'"' || !*arg ? slashes * 2 : slashes;
        while (escaped--) LMB_PROBE_CHAR(L'\\');
        if (!*arg) break;
        if (*arg == L'"') LMB_PROBE_CHAR(L'\\');
        LMB_PROBE_CHAR(*arg++);
    }
    LMB_PROBE_CHAR(L'"'); command[n] = 0; *used = n;
#undef LMB_PROBE_CHAR
    return 0;
}

static int lmb_windows_probe_capture(const char *binary, const char *const *args,
        size_t nargs, char *reply, size_t cap, size_t *length, DWORD *exit_code,
        unsigned timeout_ms) {
    if (!binary || !*binary || !reply || cap < 2 || cap > UINT32_MAX || !length ||
        !exit_code || !timeout_ms || timeout_ms > 60000 || nargs > 64 || (nargs && !args)) {
        errno = EINVAL; return -1;
    }
    *length = 0; *exit_code = MAXDWORD; reply[0] = 0;
    errno = EIO;
    wchar_t exe[2048], full[2048], argument[4096];
    wchar_t *command = calloc(32768, sizeof *command); size_t used = 0;
    int rc = -1, attr_ready = 0, assigned = 0;
    HANDLE read_end = NULL, write_end = NULL, input = INVALID_HANDLE_VALUE,
           error = INVALID_HANDLE_VALUE, job = NULL;
    PROCESS_INFORMATION child = {0}; STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof startup;
    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, NULL, TRUE};
    if (!command) { errno = ENOMEM; return -1; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, binary, -1, exe, 2048)) goto done;
    DWORD count = GetFullPathNameW(exe, 2048, full, NULL);
    if (!count || count >= 2048 || lmb_probe_arg(command, 32768, &used, full)) goto done;
    for (size_t i = 0; i < nargs; i++) {
        if (!args[i] || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, args[i], -1, argument, 4096) ||
            lmb_probe_arg(command, 32768, &used, argument)) goto done;
    }
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0) ||
        !SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) goto done;
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &inheritable, OPEN_EXISTING, 0, NULL);
    if (input == INVALID_HANDLE_VALUE) goto done;
    HANDLE current_error = GetStdHandle(STD_ERROR_HANDLE);
    if (!current_error || current_error == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), current_error, GetCurrentProcess(), &error,
                          0, TRUE, DUPLICATE_SAME_ACCESS)) {
        error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &inheritable, OPEN_EXISTING, 0, NULL);
    }
    if (error == INVALID_HANDLE_VALUE) goto done;
    SIZE_T attr_bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_bytes);
    startup.lpAttributeList = attr_bytes ? malloc(attr_bytes) : NULL;
    if (!startup.lpAttributeList || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attr_bytes)) goto done;
    attr_ready = 1;
    HANDLE handles[] = {input, write_end, error};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    handles, sizeof handles, NULL, NULL)) goto done;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input; startup.StartupInfo.hStdOutput = write_end;
    startup.StartupInfo.hStdError = error;
    job = CreateJobObjectW(NULL, NULL); if (!job) goto done;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) goto done;
    /* The engine cannot run before ownership of its entire process tree is established. */
    if (!CreateProcessW(full, command, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        NULL, NULL, &startup.StartupInfo, &child)) goto done;
    if (!AssignProcessToJobObject(job, child.hProcess)) goto done;
    assigned = 1;
    if (ResumeThread(child.hThread) == MAXDWORD) goto done;
    CloseHandle(write_end); write_end = NULL;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (GetTickCount64() >= deadline) { errno = ETIMEDOUT; goto done; }
        DWORD available = 0, got = 0;
        if (!PeekNamedPipe(read_end, NULL, 0, NULL, &available, NULL)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) goto done;
        }
        if (available) {
            if (available >= cap - *length) { errno = EMSGSIZE; goto done; }
            if (!ReadFile(read_end, reply + *length, available, &got, NULL) || !got) goto done;
            *length += got; reply[*length] = 0;
        }
        DWORD state = WaitForSingleObject(child.hProcess, 0);
        if (state == WAIT_OBJECT_0) {
            /* Drain output written between the previous peek and process exit. */
            if (!PeekNamedPipe(read_end, NULL, 0, NULL, &available, NULL)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) goto done;
            } else if (available) continue;
            if (!GetExitCodeProcess(child.hProcess, exit_code)) goto done;
            rc = 0; break;
        }
        if (state == WAIT_FAILED) goto done;
        Sleep(5);
    }
done:
    if (child.hProcess) {
        /* Only handles returned by our CreateProcess call are ever terminated. */
        if (assigned) (void)TerminateJobObject(job, 125);
        else (void)TerminateProcess(child.hProcess, 125);
        (void)WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    if (job) CloseHandle(job);
    if (read_end) CloseHandle(read_end);
    if (write_end) CloseHandle(write_end);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
    if (attr_ready) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList); free(command);
    return rc;
}
#endif
#endif
