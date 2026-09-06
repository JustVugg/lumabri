/* Child-to-supervisor readiness: never infer readiness from a port probe. */
#ifndef LUMABRI_READY_H
#define LUMABRI_READY_H
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
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
