/* A single application-owned preload library, not an arbitrary preload list.
 * Call only in the forked child immediately before exec. */
#ifndef LUMABRI_PRELOAD_H
#define LUMABRI_PRELOAD_H
#include "lumabri_platform.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int lmb_preload_file(const char *path) {
    if (!path) { errno = EINVAL; return -1; }
#ifdef __linux__
    /* ld.so splits LD_PRELOAD on whitespace and colons before opening files.
     * Quoting a path cannot fix that. An inherited read-only descriptor gives
     * the loader an unambiguous, stable path even in a spaced install folder.
     * Keep this one descriptor for the engine lifetime, including child execs;
     * process exit releases it. The original shim is still used for hashing.
     */
    if (strpbrk(path, " :\t\r\n\v\f")) {
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) return -1;
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
            close(fd); errno = EINVAL; return -1;
        }
        char reference[64];
        snprintf(reference, sizeof reference, "/proc/self/fd/%d", fd);
        if (setenv(LMB_PRELOAD_ENV, reference, 1)) { close(fd); return -1; }
        return 0;
    }
#elif defined(__APPLE__)
    /* Spaces are literal in DYLD_INSERT_LIBRARIES, but colons delimit entries.
     * Refuse that unsupported install path instead of silently skipping CAS. */
    if (strchr(path, ':')) { errno = EINVAL; return -1; }
#endif
    return setenv(LMB_PRELOAD_ENV, path, 1);
}
#endif
