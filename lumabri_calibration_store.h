/* Private filesystem persistence for the common calibration codec. */
#ifndef LUMABRI_CALIBRATION_STORE_H
#define LUMABRI_CALIBRATION_STORE_H
#include "src/runtime/lumabri_calibration_codec.h"
static LMB_UNUSED int lmb_cal_filename(const char *root, char out[69]) {
    if (!root || strnlen(root, 65) != 64) return -1;
    for (size_t i = 0; i < 64; i++)
        if (!((root[i] >= '0' && root[i] <= '9') || (root[i] >= 'a' && root[i] <= 'f'))) return -1;
    snprintf(out, 69, "%s.cal", root); return 0;
}

static LMB_UNUSED int lmb_cal_directory(const char *path, int create) {
    if (!path || !*path) return -1;
    if (create && mkdir(path, 0700) && errno != EEXIST) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(fd); return -1;
    }
    return fd;
}

/* One latest record per checkpoint. Atomic replacement never exposes a
 * partial key. Multiple writers may replace each other's latest measurement,
 * but cannot mix fields from different runs. Caller owns the private parent. */
static LMB_UNUSED int lmb_cal_store(const char *directory, const LmbCalibration *r) {
    char name[69], temp[96]; LmbBuf b = {0};
    if (!r || lmb_cal_filename(r->key.model_root, name) || lmb_cal_encode(r, &b)) return -1;
    int dir = lmb_cal_directory(directory, 1), fd = -1, rc = -1;
    if (dir < 0) { free(b.p); return -1; }
    for (unsigned i = 0; i < 128; i++) {
        snprintf(temp, sizeof temp, ".record-%ld-%u.tmp", (long)getpid(), i);
        fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd >= 0) {
        size_t at = 0;
        while (at < b.len) {
            ssize_t n = write(fd, b.p + at, b.len - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            at += (size_t)n;
        }
        int ready = at == b.len && !fsync(fd);
        if (close(fd)) ready = 0;
        if (ready && !renameat(dir, temp, dir, name)) rc = fsync(dir) ? -1 : 0;
        (void)unlinkat(dir, temp, 0);
    }
    close(dir); free(b.p); return rc;
}

/* Failure clears the caller's record: an earlier successful lookup may never
 * survive a missing, malformed or inaccessible replacement as current data. */
static LMB_UNUSED int lmb_cal_load(const char *directory, const char *root, LmbCalibration *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    char name[69];
    if (lmb_cal_filename(root, name)) return -1;
    int dir = lmb_cal_directory(directory, 0);
    if (dir < 0) return -1;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(dir);
    if (fd < 0) return -1;
    struct stat st; int rc = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1 &&
        !(st.st_mode & 077) && st.st_size >= 40 && st.st_size <= LMB_CAL_RECORD_MAX) {
        size_t len = (size_t)st.st_size, at = 0;
        uint8_t *bytes = malloc(len);
        if (bytes) {
            while (at < len) {
                ssize_t n = read(fd, bytes + at, len - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                at += (size_t)n;
            }
            uint8_t extra;
            if (at == len && read(fd, &extra, 1) == 0) rc = lmb_cal_decode(bytes, len, out);
            if (!rc && strcmp(out->key.model_root, root)) { memset(out, 0, sizeof *out); rc = -1; }
            free(bytes);
        }
    }
    close(fd); return rc;
}
#endif
