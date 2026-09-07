/* Stat-only preview: no hashing or reading model weights. The signed source
 * inventory is still authoritative and is checked again before offers. */
#ifndef LUMABRI_CHECKPOINT_INVENTORY_H
#define LUMABRI_CHECKPOINT_INVENTORY_H
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include "lumabri_content.h"

typedef struct {
    uint64_t bytes;
    uint32_t files;
    int has_weights;
} LmbCheckpointInventory;

static int lmb_checkpoint_walk(const char *root, const char *rel, unsigned depth,
                                LmbCheckpointInventory *out) {
    if (depth > 32) return -1;
    char directory[1024];
    int len = snprintf(directory, sizeof directory, "%s%s%s", root, *rel ? "/" : "", rel);
    if (len < 0 || (size_t)len >= sizeof directory) return -1;
    DIR *dir = opendir(directory);
    if (!dir) return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) rc = -1; break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") ||
            lmb_content_runtime_local_name(name)) continue;
        char child[512], path[1024];
        len = snprintf(child, sizeof child, "%s%s%s", rel, *rel ? "/" : "", name);
        if (len < 0 || (size_t)len >= sizeof child) { rc = -1; break; }
        len = snprintf(path, sizeof path, "%s/%s", root, child);
        if (len < 0 || (size_t)len >= sizeof path) { rc = -1; break; }
        struct stat link, st;
        if (lstat(path, &link) || stat(path, &st)) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            /* Weight-file symlinks (HF caches) work; directory symlinks may
             * form cycles and are not accepted by this bounded preview. */
            if (S_ISLNK(link.st_mode) || lmb_checkpoint_walk(root, child, depth + 1, out)) {
                rc = -1; break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 || out->files == LMB_CONTENT_MAX_FILES ||
                UINT64_MAX - out->bytes < (uint64_t)st.st_size) { rc = -1; break; }
            out->files++;
            out->bytes += (uint64_t)st.st_size;
            const char *dot = strrchr(name, '.');
            if (st.st_size >= 4096 && dot && (!strcmp(dot, ".safetensors") ||
                !strcmp(dot, ".bin") || !strcmp(dot, ".gguf") || !strcmp(dot, ".coli")))
                out->has_weights = 1;
        }
    }
    closedir(dir);
    return rc;
}

static int lmb_checkpoint_inventory(const char *root, LmbCheckpointInventory *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!root || !*root || lmb_checkpoint_walk(root, "", 0, out)) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return 0;
}
#endif
