/* A conservative identity for the currently running donor's launch context.
 * A new donor epoch invalidates measurements even if the binaries are equal:
 * inherited engine environment and dynamic-library state are not guessed.
 * No model is opened. Unchanged executable hashes are reused after stat checks. */
#ifndef LUMABRI_RUNTIME_IDENTITY_H
#define LUMABRI_RUNTIME_IDENTITY_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lumabri_platform.h"
#include "lumabri_sha.h"

static LMB_MAYBE_UNUSED uint64_t lmb_stat_mtime_ns(const struct stat *s) {
#ifdef __APPLE__
    return (uint64_t)s->st_mtimespec.tv_sec * 1000000000ull + (uint64_t)s->st_mtimespec.tv_nsec;
#else
    return (uint64_t)s->st_mtim.tv_sec * 1000000000ull + (uint64_t)s->st_mtim.tv_nsec;
#endif
}
static LMB_MAYBE_UNUSED uint64_t lmb_stat_ctime_ns(const struct stat *s) {
#ifdef __APPLE__
    return (uint64_t)s->st_ctimespec.tv_sec * 1000000000ull + (uint64_t)s->st_ctimespec.tv_nsec;
#else
    return (uint64_t)s->st_ctim.tv_sec * 1000000000ull + (uint64_t)s->st_ctim.tv_nsec;
#endif
}
static LMB_MAYBE_UNUSED int lmb_stat_same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mode == b->st_mode && lmb_stat_mtime_ns(a) == lmb_stat_mtime_ns(b) &&
           lmb_stat_ctime_ns(a) == lmb_stat_ctime_ns(b);
}
typedef struct { struct stat stat; uint8_t digest[32]; int valid; } LmbBinaryDigest;
typedef struct { LmbBinaryDigest files[5]; } LmbRuntimeIdentityCache;

static LMB_MAYBE_UNUSED int lmb_binary_digest(const char *path, LmbBinaryDigest *cache, uint8_t out[32]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        (uint64_t)before.st_size > (UINT64_C(1) << 30)) { close(fd); return -1; }
    if (cache->valid && lmb_stat_same(&cache->stat, &before)) {
        memcpy(out, cache->digest, 32); close(fd); return 0;
    }
    uint8_t buf[65536], digest[32]; uint64_t total = 0; LmbSha sha;
    lmb_sha_init(&sha);
    while (total < (uint64_t)before.st_size) {
        size_t want = (uint64_t)before.st_size - total > sizeof buf ? sizeof buf : (size_t)((uint64_t)before.st_size - total);
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); cache->valid = 0; return -1; }
        lmb_sha_update(&sha, buf, (size_t)n); total += (uint64_t)n;
    }
    int stable = !fstat(fd, &after) && lmb_stat_same(&before, &after);
    close(fd);
    if (!stable) { cache->valid = 0; return -1; }
    lmb_sha_final(&sha, digest); cache->stat = before;
    memcpy(cache->digest, digest, 32); cache->valid = 1;
    memcpy(out, digest, 32); return 0;
}

static LMB_MAYBE_UNUSED int lmb_runtime_identity(const char *bin_dir, const char *epoch,
    LmbRuntimeIdentityCache *cache, char out[65]) {
    out[0] = 0;
    if (!bin_dir || !epoch || strnlen(epoch, 65) != 64 || !cache) return -1;
    for (size_t i = 0; i < 64; i++)
        if (!((epoch[i] >= '0' && epoch[i] <= '9') || (epoch[i] >= 'a' && epoch[i] <= 'f'))) return -1;
    const char *names[] = {"lumabri", "segment_node", "segment_chat", LMB_SHIM_NAME};
    LmbSha sha; lmb_sha_init(&sha);
    lmb_sha_update(&sha, "lumabri-donor-runtime-v1", sizeof "lumabri-donor-runtime-v1");
    lmb_sha_update(&sha, epoch, 64);
    for (unsigned i = 0; i < 4; i++) {
        char path[1200]; uint8_t digest[32];
        int n = snprintf(path, sizeof path, "%s/%s", bin_dir, names[i]);
        if (n < 0 || (size_t)n >= sizeof path) return -1;
        if (i == 3 && access(path, R_OK)) {
            n = snprintf(path, sizeof path, "%s/../lib/lumabri/%s", bin_dir, names[i]);
            if (n < 0 || (size_t)n >= sizeof path) return -1;
        }
        if (lmb_binary_digest(path, &cache->files[i], digest)) return -1;
        lmb_sha_update(&sha, names[i], strlen(names[i]) + 1);
        lmb_sha_update(&sha, digest, 32);
    }
    /* A replaced bundled OpenMP runtime changes execution even if the four
     * executables did not change. Absence is explicit, not a stale digest.
     * Unbundled system libraries remain covered conservatively by the epoch. */
    char omp[1200]; uint8_t omp_digest[32]; struct stat omp_stat;
    int n = snprintf(omp, sizeof omp, "%s/../lib/lumabri/libomp.dylib", bin_dir);
    if (n < 0 || (size_t)n >= sizeof omp) return -1;
    lmb_sha_update(&sha, "bundled-libomp", sizeof "bundled-libomp");
    if (!lstat(omp, &omp_stat)) {
        if (!S_ISREG(omp_stat.st_mode) || lmb_binary_digest(omp, &cache->files[4], omp_digest)) return -1;
        lmb_sha_update(&sha, omp_digest, sizeof omp_digest);
    } else {
        if (errno != ENOENT) return -1;
        cache->files[4].valid = 0;
        lmb_sha_update(&sha, "absent", sizeof "absent");
    }
    uint8_t digest[32]; lmb_sha_final(&sha, digest);
    for (unsigned i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return 0;
}
#endif
