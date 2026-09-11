/* Test-only native observer: no latency injection, no changes to sync results. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int beneath(const char *path, const char *root) {
    if (!root || !*root) return 0;
    size_t n = strlen(root);
    return !strncmp(path, root, n) && (!path[n] || path[n] == '/');
}

static void observe(int fd) {
    char path[PATH_MAX];
#ifdef __APPLE__
    if (fcntl(fd, F_GETPATH, path)) return;
#else
    char link[64]; snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n < 0) return;
    path[n] = 0;
#endif
    if (beneath(path, getenv("LUMABRI_CAS"))) fputs("TEST_CAS_SYNC\n", stderr);
    if (beneath(path, getenv("LUMABRI_CACHE"))) fputs("TEST_MIRROR_SYNC\n", stderr);
}

static int traced_fsync(int fd) {
#ifdef __APPLE__
    /* As in lumashim's native interposer, references in this image retain
     * the original libc binding. RTLD_NEXT can resolve an interposed symbol
     * again on Darwin and recurse before the client can print diagnostics. */
    int (*actual)(int) = fsync;
#else
    int (*actual)(int) = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
#endif
    if (!actual) { errno = ENOSYS; return -1; }
    observe(fd);
    return actual(fd);
}

#ifdef __APPLE__
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } sync_interpose[] = {
    {(const void *)traced_fsync, (const void *)fsync}
};
#else
int fsync(int fd) { return traced_fsync(fd); }
int fdatasync(int fd) {
    int (*actual)(int) = (int (*)(int))dlsym(RTLD_NEXT, "fdatasync");
    if (!actual) { errno = ENOSYS; return -1; }
    observe(fd);
    return actual(fd);
}
#endif
