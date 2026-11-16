/* Same-computer weight source.
 *
 * When the computer that publishes a model is also one of the computers that
 * loads it, the loader used to pull every block of its own disk through the
 * encrypted peer transport on loopback. This header is the rendezvous that
 * lets it read the file instead — as a HINT, never as authority:
 *
 *   - the household launcher writes "<dir>/<model>" holding the absolute
 *     directory its source service publishes; <dir> is 0700 and the record
 *     0600, both owned by the user;
 *   - a loader given LUMABRI_LOCAL_SOURCES=<dir> accepts the record only if
 *     the directory and the record are owned by its own effective user and
 *     writable by nobody else, and a weight file only if it is a regular
 *     file of that user with exactly the size the manifest announces;
 *   - every block read that way is hashed against the same authenticated
 *     truth a block from a peer is hashed against, before use. The first
 *     mismatch retires the local file and the block comes from the peers.
 *
 * Nothing here listens, and nothing crosses the network: another computer
 * can neither publish a record nor learn that one exists. POSIX only. */
#ifndef LUMABRI_LOCAL_SOURCE_H
#define LUMABRI_LOCAL_SOURCE_H
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LMB_LOCAL_SOURCES_ENV "LUMABRI_LOCAL_SOURCES"
#define LMB_LOCAL_SOURCE_ROOT_MAX 1024

/* The record is named after the model, so the name must be one path
 * component that cannot leave the directory. */
static inline int lmb_local_source_name_ok(const char *model) {
    if (!model || !model[0] || model[0] == '.' || strlen(model) > 96) return 0;
    for (const char *p = model; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
            return 0;
    return 1;
}

/* Owned by this user and writable by nobody else. */
static inline int lmb_local_source_private(const struct stat *st) {
    return st->st_uid == geteuid() && !(st->st_mode & (S_IWGRP | S_IWOTH));
}

/* Launcher side: record that `root` is where this user publishes `model`.
 * Best effort — without a record the loader simply uses the network. */
static inline int lmb_local_source_publish(const char *dir, const char *model,
                                           const char *root) {
    char real[LMB_LOCAL_SOURCE_ROOT_MAX], path[LMB_LOCAL_SOURCE_ROOT_MAX + 160];
    char tmp[LMB_LOCAL_SOURCE_ROOT_MAX + 200], *resolved;
    struct stat st;
    if (!dir || !dir[0] || !root || !lmb_local_source_name_ok(model)) return -1;
    resolved = realpath(root, NULL);
    if (!resolved) return -1;
    int fits = strlen(resolved) < sizeof real && !strchr(resolved, '\n');
    if (fits) snprintf(real, sizeof real, "%s", resolved);
    free(resolved);
    if (!fits) return -1;
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || !lmb_local_source_private(&st)) return -1;
    if (snprintf(path, sizeof path, "%s/%s", dir, model) >= (int)sizeof path ||
        snprintf(tmp, sizeof tmp, "%s/.%s.%ld.tmp", dir, model, (long)getpid()) >= (int)sizeof tmp)
        return -1;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t n = strlen(real), put = 0;
    while (put < n) {
        ssize_t w = write(fd, real + put, n - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        put += (size_t)w;
    }
    int ok = put == n;
    if (close(fd)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

#endif /* !_WIN32 */
#endif /* LUMABRI_LOCAL_SOURCE_H */
