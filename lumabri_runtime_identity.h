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
    if (fstat(fd, &before)) { int error=errno; close(fd); errno=error; return -1; }
    if (!S_ISREG(before.st_mode) || before.st_size <= 0 ||
        (uint64_t)before.st_size > (UINT64_C(1) << 30)) { close(fd); errno=EINVAL; return -1; }
    if (cache->valid && lmb_stat_same(&cache->stat, &before)) {
        memcpy(out, cache->digest, 32); close(fd); return 0;
    }
    uint8_t buf[65536], digest[32]; uint64_t total = 0; LmbSha sha;
    lmb_sha_init(&sha);
    while (total < (uint64_t)before.st_size) {
        size_t want = (uint64_t)before.st_size - total > sizeof buf ? sizeof buf : (size_t)((uint64_t)before.st_size - total);
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int error=n<0 ? errno : EIO; close(fd); cache->valid = 0; errno=error; return -1; }
        lmb_sha_update(&sha, buf, (size_t)n); total += (uint64_t)n;
    }
    int stat_error=fstat(fd,&after) ? errno : 0;
    int stable = !stat_error && lmb_stat_same(&before, &after);
    close(fd);
    if (!stable) { cache->valid = 0; errno=stat_error ? stat_error : ESTALE; return -1; }
    lmb_sha_final(&sha, digest); cache->stat = before;
    memcpy(cache->digest, digest, 32); cache->valid = 1;
    memcpy(out, digest, 32); return 0;
}

static const char *const lmb_runtime_components[]={"lumabri","segment_node","segment_chat",LMB_SHIM_NAME,"libomp.dylib"};

static LMB_MAYBE_UNUSED int lmb_runtime_identity_details(const char *bin_dir, const char *epoch,
    LmbRuntimeIdentityCache *cache, char out[65],const char **component) {
    out[0] = 0;
    if (component) *component="launch context";
    if (!bin_dir || !epoch || strnlen(epoch, 65) != 64 || !cache) { errno=EINVAL; return -1; }
    for (size_t i = 0; i < 64; i++)
        if (!((epoch[i] >= '0' && epoch[i] <= '9') || (epoch[i] >= 'a' && epoch[i] <= 'f'))) { errno=EINVAL; return -1; }
    LmbSha sha; lmb_sha_init(&sha);
    lmb_sha_update(&sha, "lumabri-donor-runtime-v1", sizeof "lumabri-donor-runtime-v1");
    lmb_sha_update(&sha, epoch, 64);
    for (unsigned i = 0; i < 4; i++) {
        char path[1200]; uint8_t digest[32];
        if (component) *component=lmb_runtime_components[i];
        int n = snprintf(path, sizeof path, "%s/%s", bin_dir, lmb_runtime_components[i]);
        if (n < 0 || (size_t)n >= sizeof path) { errno=ENAMETOOLONG; return -1; }
        if (i == 3 && access(path, R_OK)) {
            /* A denied existing library is not a missing development-tree
             * path. Preserve the actual error instead of trying another file. */
            if (errno!=ENOENT) return -1;
            n = snprintf(path, sizeof path, "%s/../lib/lumabri/%s", bin_dir, lmb_runtime_components[i]);
            if (n < 0 || (size_t)n >= sizeof path) { errno=ENAMETOOLONG; return -1; }
        }
        if (lmb_binary_digest(path, &cache->files[i], digest)) return -1;
        lmb_sha_update(&sha, lmb_runtime_components[i], strlen(lmb_runtime_components[i]) + 1);
        lmb_sha_update(&sha, digest, 32);
    }
    /* A replaced bundled OpenMP runtime changes execution even if the four
     * executables did not change. Absence is explicit, not a stale digest.
     * Unbundled system libraries remain covered conservatively by the epoch. */
    char omp[1200]; uint8_t omp_digest[32]; struct stat omp_stat;
    if (component) *component=lmb_runtime_components[4];
    int n = snprintf(omp, sizeof omp, "%s/../lib/lumabri/libomp.dylib", bin_dir);
    if (n < 0 || (size_t)n >= sizeof omp) { errno=ENAMETOOLONG; return -1; }
    lmb_sha_update(&sha, "bundled-libomp", sizeof "bundled-libomp");
    if (!lstat(omp, &omp_stat)) {
        if (!S_ISREG(omp_stat.st_mode)) { errno=EINVAL; return -1; }
        if (lmb_binary_digest(omp, &cache->files[4], omp_digest)) return -1;
        lmb_sha_update(&sha, omp_digest, sizeof omp_digest);
    } else {
        if (errno != ENOENT) return -1;
        cache->files[4].valid = 0;
        lmb_sha_update(&sha, "absent", sizeof "absent");
    }
    uint8_t digest[32]; lmb_sha_final(&sha, digest);
    for (unsigned i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    if (component) *component=NULL;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_runtime_identity(const char *bin_dir,const char *epoch,
    LmbRuntimeIdentityCache *cache,char out[65]) {
    return lmb_runtime_identity_details(bin_dir,epoch,cache,out,NULL);
}

/* Read failures and actual content changes need different recovery advice.
 * Do not print paths/keys to the requester, or accept a new digest in place
 * of the immutable launch identity. */
static LMB_MAYBE_UNUSED int lmb_runtime_identity_check(const char *bin_dir,const char *epoch,
    LmbRuntimeIdentityCache *cache,const char expected[65],char *error,size_t size) {
    if (size) error[0]=0;
    LmbRuntimeIdentityCache previous=*cache;
    char current[65]; const char *component=NULL;
    if (lmb_runtime_identity_details(bin_dir,epoch,cache,current,&component)) {
        int saved=errno;
        snprintf(error,size,"Cannot verify runtime component %s: %s. Run doctor --household.",
            component ? component : "unknown",strerror(saved));
        errno=saved; return -1;
    }
    if (!strcmp(current,expected)) return 0;
    component="launch context";
    for (unsigned i=0;i<5;i++) {
        if (previous.files[i].valid!=cache->files[i].valid ||
            (previous.files[i].valid && memcmp(previous.files[i].digest,cache->files[i].digest,32))) {
            component=lmb_runtime_components[i]; break;
        }
    }
    snprintf(error,size,"Runtime %s changed since sharing started. Restart Share resources from the intended package.",component);
    errno=ESTALE; return -1;
}
#endif
