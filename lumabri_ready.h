/* Child-to-supervisor readiness: never infer readiness from a port probe. */
#ifndef LUMABRI_READY_H
#define LUMABRI_READY_H
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>

static int lmb_ready_notify(void) {
    char value[32];
    DWORD length = GetEnvironmentVariableA("LUMABRI_READY_HANDLE", value, sizeof value);
    if (!length) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? 0 : -1;
    if (length >= sizeof value) return -1;
    uintptr_t raw = 0;
    for (DWORD i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9' || raw > (UINTPTR_MAX - (unsigned)(value[i] - '0')) / 10) return -1;
        raw = raw * 10 + (unsigned)(value[i] - '0');
    }
    if (!raw || raw == UINTPTR_MAX) return -1;
    HANDLE ready = (HANDLE)raw;
    int rc = SetEvent(ready) ? 0 : -1;
    CloseHandle(ready);
    SetEnvironmentVariableA("LUMABRI_READY_HANDLE", NULL);
    return rc;
}
#else
#include <unistd.h>
#include <fcntl.h>

/* Linux can create CLOEXEC descriptors atomically. macOS has no pipe2;
 * configure both ends before exposing them to the launch code. The portable
 * fallback, like pipe()+fcntl() generally, requires no concurrent fork/exec
 * between these calls. Keep the atomic path on Linux. */
static inline int lmb_ready_pipe(int fd[2]) {
#if defined(__linux__) && defined(_GNU_SOURCE) && !defined(LMB_READY_PORTABLE_PIPE)
    return pipe2(fd, O_CLOEXEC);
#else
    if (pipe(fd)) return -1;
    if (fcntl(fd[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd[1], F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fd[0]); close(fd[1]); fd[0] = fd[1] = -1;
        errno = saved;
        return -1;
    }
    return 0;
#endif
}

static int lmb_ready_notify(void) {
    const char *s = getenv("LUMABRI_READY_FD");
    if (!s || !*s) return 0;
    char *end;
    long fd = strtol(s, &end, 10);
    if (*end || fd < 3 || fd > INT_MAX) return -1;
    int rc = write((int)fd, "R", 1) == 1 ? 0 : -1;
    close((int)fd);
    unsetenv("LUMABRI_READY_FD");
    return rc;
}
#endif
#endif
