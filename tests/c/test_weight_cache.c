#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "src/runtime/lumabri_weight_cache.h"

static void file_at(int base, const char *name) {
    int fd = openat(base, name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "keep", 4) == 4 && !close(fd));
}

static int cancel_inspection(void *arg, const LmbWeightCacheUsage *usage, int erase) {
    (void)arg; (void)usage; assert(!erase); return 1;
}

static int cancel_clearing(void *arg, const LmbWeightCacheUsage *usage, int erase) {
    int *calls = arg;
    (*calls)++;
    return erase && usage->entries >= 64;
}

int main(void) {
    char path[] = "/tmp/lumabri-weight-cache-XXXXXX";
    assert(mkdtemp(path));
    int base = open(path, O_DIRECTORY | O_RDONLY);
    assert(base >= 0);
    assert(!mkdirat(base, "cas", 0700));
    assert(!mkdirat(base, "mirrors", 0700));
    assert(!mkdirat(base, "mirrors/olmoe", 0700));
    file_at(base, "settings"); file_at(base, "engines.log");
    file_at(base, "source.safetensors");
    file_at(base, "cas/chunk"); file_at(base, "mirrors/olmoe/config.json");
    int sparse = openat(base, "mirrors/olmoe/sparse", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(sparse >= 0 && !ftruncate(sparse, 1u << 24) && !close(sparse));
    /* Neither leaf links nor hardlinks grant ownership of their targets. */
    assert(!symlinkat("../source.safetensors", base, "cas/link"));
    assert(!linkat(base, "source.safetensors", base, "cas/hardlink", 0));
    LmbWeightCacheUsage usage;
    assert(!lmb_weight_cache_inspect(path, &usage));
    assert(usage.files == 5 && usage.logical_bytes >= (1u << 24));
    assert(usage.allocated_bytes < usage.logical_bytes);
    int lease = lmb_weight_cache_lock(path);
    assert(lease >= 0 && (fcntl(lease, F_GETFD) & FD_CLOEXEC));
    assert(lmb_weight_cache_lock(path) < 0);
    assert(lmb_weight_cache_clear(path) == -2);
    assert(!faccessat(base, "cas/chunk", F_OK, 0));
    close(lease);

    /* Preflight checks BOTH roots before deleting anything. */
    assert(!renameat(base, "mirrors", base, "saved"));
    assert(!symlinkat("saved", base, "mirrors"));
    assert(lmb_weight_cache_clear(path) == -1);
    assert(!faccessat(base, "cas/chunk", F_OK, 0));
    assert(!unlinkat(base, "mirrors", 0) && !renameat(base, "saved", base, "mirrors"));
    assert(!mkfifoat(base, "mirrors/fifo", 0600));
    assert(lmb_weight_cache_clear(path) == -1);
    assert(!faccessat(base, "cas/chunk", F_OK, 0));
    assert(!unlinkat(base, "mirrors/fifo", 0));

    assert(lmb_weight_cache_inspect_progress(path, &usage, cancel_inspection, NULL) == -1);
    assert(errno == ECANCELED && !usage.files);
    assert(lmb_weight_cache_clear_progress(path, cancel_inspection, NULL) == -1);
    assert(errno == ECANCELED && !faccessat(base, "cas/chunk", F_OK, 0));
    lease = lmb_weight_cache_lock(path); assert(lease >= 0); close(lease);
    for (unsigned i = 0; i < 130; i++) {
        char name[64]; snprintf(name, sizeof name, "cas/extra-%u", i); file_at(base, name);
    }
    int calls = 0;
    assert(lmb_weight_cache_clear_progress(path, cancel_clearing, &calls) == -1);
    assert(errno == ECANCELED && calls > 1);
    assert(!lmb_weight_cache_inspect(path, &usage) && usage.files > 0 && usage.files < 135);
    lease = lmb_weight_cache_lock(path); assert(lease >= 0); close(lease);

    assert(!lmb_weight_cache_clear(path));
    assert(!lmb_weight_cache_inspect(path, &usage) && !usage.files && !usage.allocated_bytes);
    assert(!faccessat(base, "settings", F_OK, 0));
    assert(!faccessat(base, "engines.log", F_OK, 0));
    assert(!faccessat(base, "source.safetensors", F_OK, 0));
    assert(!faccessat(base, "weights.lock", F_OK, 0));
    int source = openat(base, "source.safetensors", O_RDONLY); char text[4];
    assert(source >= 0 && read(source, text, 4) == 4 && !memcmp(text, "keep", 4));
    close(source);
    assert(!lmb_weight_cache_clear(path));
    assert(!unlinkat(base, "weights.lock", 0));
    assert(!symlinkat("settings", base, "weights.lock"));
    assert(lmb_weight_cache_clear(path) < 0);
    assert(!unlinkat(base, "weights.lock", 0));
    assert(!linkat(base, "settings", base, "weights.lock", 0));
    errno = EAGAIN;
    assert(lmb_weight_cache_clear(path) == -1 && errno == EINVAL);
    assert(!unlinkat(base, "weights.lock", 0));
    assert(!unlinkat(base, "settings", 0)); assert(!unlinkat(base, "engines.log", 0));
    assert(!unlinkat(base, "source.safetensors", 0));
    assert(!unlinkat(base, "cas", AT_REMOVEDIR)); assert(!unlinkat(base, "mirrors", AT_REMOVEDIR));
    close(base); assert(!rmdir(path));
    assert(!lmb_weight_cache_inspect(path, &usage) && !usage.files);
    assert(!lmb_weight_cache_clear(path) && access(path, F_OK));
    assert(lmb_weight_cache_clear(NULL) < 0);
    assert(lmb_weight_cache_inspect(path, NULL) < 0);
    puts("WEIGHT CACHE: PASS (exclusive lease, scoped removal, links, sparse accounting, preflight, cancellation)");
    return 0;
}
