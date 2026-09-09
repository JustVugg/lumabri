/* Household-owned weight trees only. Never traverse symlinks or clear a
 * cache while an approved plan holds its exclusive weight lease. */
#ifndef LUMABRI_WEIGHT_CACHE_H
#define LUMABRI_WEIGHT_CACHE_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint64_t allocated_bytes, logical_bytes, files;
    uint32_t entries;
} LmbWeightCacheUsage;

/* Called between filesystem operations, never while inside one. A nonzero
 * result cancels cooperatively; clearing may then be partially complete. */
typedef int (*LmbWeightCacheProgress)(void *, const LmbWeightCacheUsage *, int);

static int lmb_weight_cache_progress(LmbWeightCacheProgress progress, void *arg,
                                    const LmbWeightCacheUsage *usage, int erase) {
    if (progress && progress(arg, usage, erase)) { errno = ECANCELED; return -1; }
    return 0;
}

static int lmb_weight_cache_lock_at(int base) {
    int fd = openat(base, "weights.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) goto fail;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) { errno = EINVAL; goto fail; }
    if (st.st_uid != geteuid()) { errno = EPERM; goto fail; }
    if (flock(fd, LOCK_EX | LOCK_NB)) goto fail;
    return fd;
fail: {
    int saved = errno; close(fd); errno = saved; return -1;
}
}

static int lmb_weight_cache_lock(const char *path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    int base = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (base < 0) return -1;
    int fd = lmb_weight_cache_lock_at(base), saved = errno;
    close(base); errno = saved; return fd;
}

/* Relative descriptor traversal keeps links inside weight trees from naming
 * source checkpoints or unrelated files. Cross-device trees are refused.
 * Bounds apply to both inspection and deletion; a full inspection precedes
 * the destructive pass. Failure can still leave a partially cleared cache
 * (e.g. an I/O error), which callers must report rather than claim success. */
static int lmb_weight_cache_walk(int fd, dev_t device, unsigned depth,
                                 int erase, LmbWeightCacheUsage *usage,
                                 LmbWeightCacheProgress progress, void *arg) {
    if (depth > 32) { errno = ELOOP; return -1; }
    if (lmb_weight_cache_progress(progress, arg, usage, erase)) return -1;
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (copy < 0) return -1;
    DIR *dir = fdopendir(copy);
    if (!dir) { int saved = errno; close(copy); errno = saved; return -1; }
    int bad = 0, saved = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) bad = 1; break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (++usage->entries > 10000000) { errno = E2BIG; bad = 1; break; }
        if (!(usage->entries % 64) &&
            lmb_weight_cache_progress(progress, arg, usage, erase)) { bad = 1; break; }
        struct stat st;
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) { bad = 1; break; }
        if (st.st_dev != device || st.st_uid != geteuid() ||
            (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))) {
            errno = EPERM; bad = 1; break;
        }
        if (st.st_blocks < 0 || st.st_size < 0 ||
            (uint64_t)st.st_blocks > UINT64_MAX / 512 ||
            UINT64_MAX - usage->allocated_bytes < (uint64_t)st.st_blocks * 512 ||
            UINT64_MAX - usage->logical_bytes < (uint64_t)st.st_size) {
            errno = EOVERFLOW; bad = 1; break;
        }
        usage->allocated_bytes += (uint64_t)st.st_blocks * 512;
        usage->logical_bytes += (uint64_t)st.st_size;
        if (!S_ISDIR(st.st_mode)) usage->files++;
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            struct stat opened;
            if (child < 0) { bad = 1; break; }
            if (fstat(child, &opened) || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) {
                close(child); errno = EAGAIN; bad = 1; break;
            }
            bad = lmb_weight_cache_walk(child, device, depth + 1, erase, usage, progress, arg);
            saved = errno; close(child); errno = saved;
            if (bad) break;
        }
        if (erase && unlinkat(fd, name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0)) {
            bad = 1; break;
        }
    }
    saved = errno; closedir(dir); errno = saved;
    return bad ? -1 : 0;
}

static int lmb_weight_cache_trees(int base, int erase, LmbWeightCacheUsage *usage,
                                LmbWeightCacheProgress progress, void *arg) {
    static const char *names[] = {"cas", "mirrors"};
    struct stat st;
    if (fstat(base, &st) || st.st_uid != geteuid()) { errno = EPERM; return -1; }
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        int tree = openat(base, names[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (tree < 0) { if (errno == ENOENT) continue; return -1; }
        struct stat root;
        int bad = fstat(tree, &root) || root.st_dev != st.st_dev || root.st_uid != geteuid();
        if (bad) errno = EPERM;
        else bad = lmb_weight_cache_walk(tree, st.st_dev, 0, erase, usage, progress, arg);
        int saved = errno; close(tree); errno = saved;
        if (bad) return -1;
        /* Keep the two stable roots: the loader can safely reuse them. */
    }
    return 0;
}

static int lmb_weight_cache_inspect_progress(const char *path, LmbWeightCacheUsage *usage,
                                           LmbWeightCacheProgress progress, void *arg) {
    if (!path || !*path || !usage) { errno = EINVAL; return -1; }
    memset(usage, 0, sizeof *usage);
    int base = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (base < 0) return errno == ENOENT ? 0 : -1;
    int rc = lmb_weight_cache_trees(base, 0, usage, progress, arg), saved = errno;
    close(base); errno = saved;
    return rc;
}

static int lmb_weight_cache_clear_progress(const char *path,
                                         LmbWeightCacheProgress progress, void *arg) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    int base = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (base < 0) return errno == ENOENT ? 0 : -1;
    int lease = lmb_weight_cache_lock_at(base);
    if (lease < 0) {
        int saved = errno; close(base); errno = saved;
        return saved == EWOULDBLOCK || saved == EAGAIN ? -2 : -1;
    }
    LmbWeightCacheUsage before = {0}, removed = {0};
    int rc = lmb_weight_cache_trees(base, 0, &before, progress, arg);
    if (!rc) rc = lmb_weight_cache_trees(base, 1, &removed, progress, arg);
    int saved = errno; close(lease); close(base); errno = saved;
    return rc;
}

static inline int lmb_weight_cache_inspect(const char *path, LmbWeightCacheUsage *usage) {
    return lmb_weight_cache_inspect_progress(path, usage, NULL, NULL);
}

static inline int lmb_weight_cache_clear(const char *path) {
    return lmb_weight_cache_clear_progress(path, NULL, NULL);
}
#endif
