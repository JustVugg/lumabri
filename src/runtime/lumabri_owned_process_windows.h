/* Windows engine ownership. Handles, never saved PIDs, authorize control.
 * Windows 10 1607+ is required for atomic job assignment at process creation. */
#ifndef LMB_OWNED_PROCESS_WINDOWS_H
#define LMB_OWNED_PROCESS_WINDOWS_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    const char *name; /* Child environment variable containing its handle. */
    HANDLE handle;    /* Borrowed; never change its inheritance flags. */
} LmbChildHandle;
typedef struct {
    HANDLE process, job, stop;
    DWORD pid; /* Observation only. No operation reopens it. */
} LmbOwnedProcess;
typedef struct {
    const char *binary;
    const char *const *args;
    size_t nargs;
    const char *const *environment; /* NAME=value overrides; parent unchanged. */
    size_t nenvironment;
    HANDLE input, output, error; /* NULL means NUL. Borrowed. */
    const LmbChildHandle *handles;
    size_t nhandles;
    HANDLE stop; /* Optional cooperative event, also named in handles. */
} LmbProcessLaunch;

static inline int lmb_process_arg(wchar_t *command, size_t cap, size_t *used, const wchar_t *arg) {
    size_t n = *used;
#define LMB_ARG_CHAR(c) do { if (n + 1 >= cap) return -1; command[n++] = (c); } while (0)
    if (n) LMB_ARG_CHAR(L' ');
    LMB_ARG_CHAR(L'"');
    while (*arg) {
        size_t slashes = 0;
        while (*arg == L'\\') { slashes++; arg++; }
        size_t escaped = *arg == L'"' || !*arg ? slashes * 2 : slashes;
        while (escaped--) LMB_ARG_CHAR(L'\\');
        if (!*arg) break;
        if (*arg == L'"') LMB_ARG_CHAR(L'\\');
        LMB_ARG_CHAR(*arg++);
    }
    LMB_ARG_CHAR(L'"'); command[n] = 0; *used = n;
#undef LMB_ARG_CHAR
    return 0;
}

static inline wchar_t *lmb_process_wide(const char *text) {
    if (!text) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (n <= 0 || n > 32768) return NULL;
    wchar_t *wide = malloc((size_t)n * sizeof *wide);
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n)) {
        free(wide); wide = NULL;
    }
    return wide;
}
static inline size_t lmb_process_env_key(const wchar_t *entry) {
    /* Preserve Windows' hidden =C: drive-directory entries. */
    const wchar_t *end = wcschr(entry + (*entry == L'='), L'=');
    return end ? (size_t)(end - entry) : 0;
}
static inline int lmb_process_env_same(const wchar_t *a, const wchar_t *b) {
    size_t n = lmb_process_env_key(a);
    return n && n == lmb_process_env_key(b) && !_wcsnicmp(a, b, n);
}
static inline int lmb_process_env_order(const void *a, const void *b) {
    return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}
static inline int lmb_process_handle_name(const char *s) {
    if (!s || !*s || strlen(s) > 128) return 0;
    for (; *s; s++) if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
                          (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}

static inline wchar_t *lmb_process_environment(const LmbProcessLaunch *launch, const HANDLE *handles) {
    wchar_t *changes[128 + 16] = {0}, *result = NULL;
    wchar_t **entries = NULL; LPWCH inherited = NULL;
    size_t changed = launch->nenvironment + launch->nhandles, count = 0, total = 1;
    for (size_t i = 0; i < changed; i++) {
        if (i < launch->nenvironment) changes[i] = lmb_process_wide(launch->environment[i]);
        else {
            size_t h = i - launch->nenvironment; char value[180];
            if (!lmb_process_handle_name(launch->handles[h].name)) goto done;
            int n = snprintf(value, sizeof value, "%s=%llu", launch->handles[h].name,
                             (unsigned long long)(uintptr_t)handles[h + 3]);
            if (n < 0 || (size_t)n >= sizeof value) goto done;
            changes[i] = lmb_process_wide(value);
        }
        if (!changes[i] || *changes[i] == L'=' || !lmb_process_env_key(changes[i])) goto done;
        for (size_t j = 0; j < i; j++) if (lmb_process_env_same(changes[i], changes[j])) goto done;
    }
    inherited = GetEnvironmentStringsW(); if (!inherited) goto done;
    for (const wchar_t *p = inherited; *p; p += wcslen(p) + 1) {
        if (++count > 8192 || wcslen(p) > 32767) goto done;
    }
    entries = calloc(count + changed + 1, sizeof *entries); if (!entries) goto done;
    count = 0;
    for (wchar_t *p = inherited; *p; p += wcslen(p) + 1) {
        int replaced = 0;
        for (size_t j = 0; j < changed; j++) if (lmb_process_env_same(p, changes[j])) { replaced = 1; break; }
        if (!replaced) entries[count++] = p;
    }
    for (size_t j = 0; j < changed; j++) entries[count++] = changes[j];
    qsort(entries, count, sizeof *entries, lmb_process_env_order);
    for (size_t j = 0; j < count; j++) {
        total += wcslen(entries[j]) + 1;
        if (total > 524288) goto done;
    }
    result = calloc(total + 1, sizeof *result);
    if (result) {
        size_t at = 0;
        for (size_t j = 0; j < count; j++) {
            size_t n = wcslen(entries[j]) + 1;
            memcpy(result + at, entries[j], n * sizeof *result); at += n;
        }
    }
done:
    if (inherited) FreeEnvironmentStringsW(inherited);
    for (size_t j = 0; j < changed; j++) free(changes[j]);
    free(entries); return result;
}

/* 0 = entire owned job exited, 1 = some descendant is still alive. */
static inline int lmb_owned_process_alive(const LmbOwnedProcess *p) {
    if (!p || !p->job || !p->process) { errno = EINVAL; return -1; }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {0};
    if (!QueryInformationJobObject(p->job, JobObjectBasicAccountingInformation, &info, sizeof info, NULL)) {
        errno = EIO; return -1;
    }
    return info.ActiveProcesses != 0;
}
static inline int lmb_owned_process_wait(const LmbOwnedProcess *p, unsigned timeout_ms, DWORD *exit_code) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        int alive = lmb_owned_process_alive(p);
        if (alive < 0) return -1;
        if (!alive) {
            DWORD status;
            if (!GetExitCodeProcess(p->process, &status)) { errno = EIO; return -1; }
            if (exit_code) *exit_code = status;
            return 0;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return 1;
        Sleep((DWORD)(deadline - now < 10 ? deadline - now : 10));
    }
}
static inline int lmb_owned_process_close(LmbOwnedProcess *p) {
    int alive = lmb_owned_process_alive(p);
    if (alive) { if (alive > 0) errno = EBUSY; return -1; }
    if (p->stop) CloseHandle(p->stop);
    CloseHandle(p->process); CloseHandle(p->job); memset(p, 0, sizeof *p); return 0;
}
static inline int lmb_owned_process_stop(LmbOwnedProcess *p, unsigned grace_ms, unsigned kill_wait_ms) {
    if (!p || !p->process || !p->job || grace_ms > 60000 || kill_wait_ms > 60000) { errno = EINVAL; return -1; }
    int alive = lmb_owned_process_alive(p);
    if (alive <= 0) return alive;
    if (p->stop && !SetEvent(p->stop)) { errno = EIO; return -1; }
    int rc = lmb_owned_process_wait(p, grace_ms, NULL);
    if (!rc) return 0;
    if (rc < 0 || !TerminateJobObject(p->job, 125)) { errno = EIO; return -1; }
    rc = lmb_owned_process_wait(p, kill_wait_ms, NULL);
    if (rc > 0) errno = ETIMEDOUT;
    /* On an unconfirmed stop, keep the job/process handles and reservation. */
    return rc ? -1 : 0;
}

static inline int lmb_owned_process_start(LmbOwnedProcess *p, const LmbProcessLaunch *launch) {
    if (!p || !launch || p->job || p->process || p->stop || !launch->binary || !*launch->binary ||
        launch->nargs > 64 || (launch->nargs && !launch->args) || launch->nenvironment > 128 ||
        (launch->nenvironment && !launch->environment) || launch->nhandles > 16 ||
        (launch->nhandles && !launch->handles)) { errno = EINVAL; return -1; }
    int rc = -1, attr_ready = 0; size_t used = 0, nh = 3 + launch->nhandles;
    wchar_t *binary = lmb_process_wide(launch->binary), *environment = NULL;
    wchar_t full[2048], *command = calloc(32768, sizeof *command);
    HANDLE handles[19] = {0}, job = NULL, stop = NULL;
    STARTUPINFOEXW startup = {0}; PROCESS_INFORMATION child = {0};
    startup.StartupInfo.cb = sizeof startup; errno = EIO;
    if (!binary || !command) goto done;
    DWORD n = GetFullPathNameW(binary, 2048, full, NULL);
    if (!n || n >= 2048 || lmb_process_arg(command, 32768, &used, full)) goto done;
    for (size_t i = 0; i < launch->nargs; i++) {
        wchar_t *arg = lmb_process_wide(launch->args[i]);
        int bad = !arg || lmb_process_arg(command, 32768, &used, arg); free(arg);
        if (bad) goto done;
    }
    HANDLE standard[] = {launch->input, launch->output, launch->error};
    SECURITY_ATTRIBUTES inherit = {sizeof inherit, NULL, TRUE};
    for (size_t i = 0; i < nh; i++) {
        HANDLE source = i < 3 ? standard[i] : launch->handles[i - 3].handle;
        if (!source && i < 3) handles[i] = CreateFileW(L"NUL", i ? GENERIC_WRITE : GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
        else if (!source || source == INVALID_HANDLE_VALUE || !DuplicateHandle(GetCurrentProcess(), source,
            GetCurrentProcess(), &handles[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) goto done;
        if (handles[i] == INVALID_HANDLE_VALUE) { handles[i] = NULL; goto done; }
    }
    environment = lmb_process_environment(launch, handles); if (!environment) { errno = EINVAL; goto done; }
    if (launch->stop && !DuplicateHandle(GetCurrentProcess(), launch->stop, GetCurrentProcess(), &stop,
                                       0, FALSE, DUPLICATE_SAME_ACCESS)) goto done;
    job = CreateJobObjectW(NULL, NULL); if (!job) goto done;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) goto done;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 2, 0, &bytes);
    startup.lpAttributeList = bytes ? malloc(bytes) : NULL;
    if (!startup.lpAttributeList || !InitializeProcThreadAttributeList(startup.lpAttributeList, 2, 0, &bytes)) goto done;
    attr_ready = 1;
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles, nh * sizeof *handles, NULL, NULL) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &job, sizeof job, NULL, NULL)) goto done;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0]; startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError = handles[2];
    /* Atomic assignment closes the owner-crash gap between CreateProcess and
     * AssignProcessToJobObject. Do not fall back to an unowned child. */
    if (!CreateProcessW(full, command, NULL, NULL, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        environment, NULL, &startup.StartupInfo, &child)) goto done;
    if (ResumeThread(child.hThread) == MAXDWORD) goto done;
    p->process = child.hProcess; p->job = job; p->stop = stop; p->pid = child.dwProcessId;
    child.hProcess = job = stop = NULL; rc = 0;
done:
    if (child.hProcess) {
        if (job) (void)TerminateJobObject(job, 125);
        (void)WaitForSingleObject(child.hProcess, 5000); CloseHandle(child.hProcess);
    }
    if (child.hThread) CloseHandle(child.hThread);
    if (job) CloseHandle(job);
    if (stop) CloseHandle(stop);
    for (size_t i = 0; i < nh; i++) if (handles[i]) CloseHandle(handles[i]);
    if (attr_ready) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList); free(environment); free(command); free(binary); return rc;
}
#endif
#endif
