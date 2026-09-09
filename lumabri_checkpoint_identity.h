/* Lightweight identity lookup from the maintainer's existing hash sidecars.
 * No weight bytes are read here. Missing/stale sidecars mean unknown, never
 * a guessed identity. Execution still checks the signed routing root. */
#ifndef LUMABRI_CHECKPOINT_IDENTITY_H
#define LUMABRI_CHECKPOINT_IDENTITY_H
#include "lumabri_checkpoint_inventory.h"
#include "lumabri_runtime_identity.h"

typedef struct {
    LmbModelItem items[LMB_CONTENT_MAX_FILES];
    size_t count, hash_bytes;
} LmbCheckpointHashes;

static LMB_MAYBE_UNUSED int lmb_checkpoint_cached_hash(const char *root, const char *rel,
    const struct stat *observed, void *arg) {
    LmbCheckpointHashes *h = arg;
    char path[1600];
    int n = snprintf(path, sizeof path, "%s/%s", root, rel);
    if (n < 0 || (size_t)n >= sizeof path || h->count >= LMB_CONTENT_MAX_FILES) return -1;
    int weight = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (weight < 0) return -1;
    struct stat before, after;
    if (fstat(weight, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        !lmb_stat_same(observed, &before)) { close(weight); return -1; }
    uint64_t chunks = ((uint64_t)before.st_size + (1u << 20) - 1) >> 20;
    if (chunks > ((64u << 20) - h->hash_bytes) / 32) { close(weight); return -1; }
    n = snprintf(path, sizeof path, "%s/.lumabri_hashes/%s.sha", root, rel);
    if (n < 0 || (size_t)n >= sizeof path) { close(weight); return -1; }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { close(weight); return -1; }
    /* This is the local LSH1 sidecar written by maintainer.c, not a new wire
     * format. All supported source platforms use its same fixed layout. */
    struct {
        uint32_t magic, version;
        uint64_t size, mtime_ns, ctime_ns;
        uint32_t nh, reserved;
    } hdr;
    _Static_assert(sizeof hdr == 40, "unexpected LSH1 sidecar layout");
    struct stat sidecar;
    int valid = !fstat(fd, &sidecar) && S_ISREG(sidecar.st_mode) &&
        sidecar.st_size == (off_t)(sizeof hdr + chunks * 32);
    size_t bytes = sizeof hdr + (size_t)chunks * 32, at = 0;
    uint8_t *data = valid ? malloc(bytes) : NULL;
    if (data) while (at < bytes) {
        ssize_t got = read(fd, data + at, bytes - at);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        at += (size_t)got;
    }
    close(fd);
    valid = data && at == bytes && !fstat(weight, &after) && lmb_stat_same(&before, &after);
    close(weight);
    if (!valid) { free(data); return -1; }
    memcpy(&hdr, data, sizeof hdr);
    if (hdr.magic != 0x3148534Cu || hdr.version != 1 || hdr.reserved ||
        hdr.size != (uint64_t)before.st_size || hdr.nh != chunks ||
        hdr.mtime_ns != lmb_stat_mtime_ns(&before) || hdr.ctime_ns != lmb_stat_ctime_ns(&before)) {
        free(data); return -1;
    }
    char *name = strdup(rel);
    uint8_t *hashes = malloc((size_t)chunks * 32 + 1);
    if (!name || !hashes) { free(name); free(hashes); free(data); return -1; }
    memcpy(hashes, data + sizeof hdr, (size_t)chunks * 32); free(data);
    h->items[h->count++] = (LmbModelItem){name, hdr.size, hdr.nh, hashes};
    h->hash_bytes += (size_t)chunks * 32;
    return 0;
}

static LMB_MAYBE_UNUSED int lmb_checkpoint_identity(const char *directory, char content[65],
    const char *routing_name, uint8_t routing_root[32]) {
    if (!content) return -1;
    content[0] = 0;
    if (routing_root) memset(routing_root, 0, 32);
    if (!directory || !*directory || (!!routing_name != !!routing_root)) return -1;
    LmbCheckpointHashes *hashes = calloc(1, sizeof *hashes);
    if (!hashes) return -1;
    LmbCheckpointInventory inventory = {0}; uint8_t digest[32];
    int bad = lmb_checkpoint_walk(directory, "", 0, &inventory, lmb_checkpoint_cached_hash, hashes) ||
        !hashes->count || lmb_model_root("lumabri-calibration-content-v1", hashes->items, hashes->count, digest);
    if (!bad && routing_name) bad = lmb_model_root(routing_name, hashes->items, hashes->count, routing_root);
    if (!bad) for (unsigned i = 0; i < 32; i++) snprintf(content + 2 * i, 3, "%02x", digest[i]);
    for (size_t i = 0; i < hashes->count; i++) {
        free((void *)hashes->items[i].path); free((void *)hashes->items[i].hashes);
    }
    free(hashes); return bad ? -1 : 0;
}
#endif
