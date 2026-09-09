/* Private, bounded calibration records. The checksum detects torn/corrupt
 * records, not dishonest hosts. No prompt, reply, token or credential is
 * stored. Publication to the catalogue still requires an exact valid key. */
#ifndef LUMABRI_CALIBRATION_STORE_H
#define LUMABRI_CALIBRATION_STORE_H
#include <float.h>
#include <sys/stat.h>
#include "lumabri_proto.h"
#include "lumabri_sha.h"
#include "lumabri_calibration.h"

#define LMB_CAL_RECORD_MAX 32768u
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "calibration records require binary64 doubles");

static LMB_UNUSED int lmb_cal_put_double(LmbBuf *b, double value) {
    uint64_t bits; memcpy(&bits, &value, sizeof bits);
    return lmb_buf_u64(b, bits);
}
static LMB_UNUSED int lmb_cal_get_double(LmbCur *c, double *value) {
    uint64_t bits;
    if (lmb_cur_u64(c, &bits)) return -1;
    memcpy(value, &bits, sizeof bits); return 0;
}
static LMB_UNUSED int lmb_cal_get_text(LmbCur *c, char *out, size_t cap) {
    uint16_t n;
    if (lmb_cur_u16(c, &n) || n >= cap || c->off > c->len ||
        n > c->len - c->off || memchr(c->p + c->off, 0, n)) return -1;
    memcpy(out, c->p + c->off, n); out[n] = 0; c->off += n;
    return !n || lmb_cal_text(out, cap) ? 0 : -1;
}

static LMB_UNUSED int lmb_cal_encode(const LmbCalibration *r, LmbBuf *out) {
    if (!out || out->len || !lmb_cal_valid(r)) return -1;
    const LmbCalKey *k = &r->key;
    LmbBuf b = {0}; uint8_t digest[32]; LmbSha sha;
#define PUT(call) do { if (call) goto bad; } while (0)
#define STR(f) PUT(lmb_buf_str(&b, k->f))
#define U32(f) PUT(lmb_buf_u32(&b, k->f))
    PUT(lmb_buf_bytes(&b, "LMB-CAL1", 8));
    STR(model_root); STR(adapter); U32(adapter_abi); STR(numeric_class);
    STR(commit_lumabri); STR(commit_colibri); STR(build_id); STR(plan_kind);
    U32(goal); U32(nodes); U32(edge_node); U32(context); U32(sessions);
    for (uint32_t i = 0; i < k->nodes; i++) {
        STR(node_id[i]); STR(node_hardware_id[i]); STR(node_build_id[i]); STR(node_backend[i]);
        U32(layer_begin[i]); U32(layer_end[i]); U32(threads[i]); U32(from_disk[i]);
    }
    PUT(lmb_cal_put_double(&b, r->decode_tok_s));
    PUT(lmb_cal_put_double(&b, r->ttft_seconds));
    PUT(lmb_cal_put_double(&b, r->measured_at));
    PUT(lmb_buf_u32(&b, r->samples));
    PUT(lmb_buf_u32(&b, r->prompt_tokens));
    PUT(lmb_buf_u32(&b, r->generated_tokens));
    lmb_sha_init(&sha); lmb_sha_update(&sha, b.p, b.len); lmb_sha_final(&sha, digest);
    PUT(lmb_buf_bytes(&b, digest, sizeof digest));
    if (b.len > LMB_CAL_RECORD_MAX) goto bad;
    free(out->p); *out = b; return 0;
bad:
    free(b.p); return -1;
#undef PUT
#undef STR
#undef U32
}

static LMB_UNUSED int lmb_cal_decode(const void *data, size_t len, LmbCalibration *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!data || len < 40 || len > LMB_CAL_RECORD_MAX || memcmp(data, "LMB-CAL1", 8)) return -1;
    uint8_t digest[32]; LmbSha sha;
    lmb_sha_init(&sha); lmb_sha_update(&sha, data, len - 32); lmb_sha_final(&sha, digest);
    if (memcmp(digest, (const uint8_t *)data + len - 32, 32)) return -1;
    LmbCur c = {(const uint8_t *)data, len - 32, 8};
    LmbCalibration r = {0}; LmbCalKey *k = &r.key;
#define GET(call) do { if (call) return -1; } while (0)
#define STR(f) GET(lmb_cal_get_text(&c, k->f, sizeof k->f))
#define U32(f) GET(lmb_cur_u32(&c, &k->f))
    STR(model_root); STR(adapter); U32(adapter_abi); STR(numeric_class);
    STR(commit_lumabri); STR(commit_colibri); STR(build_id); STR(plan_kind);
    U32(goal); U32(nodes); U32(edge_node); U32(context); U32(sessions);
    if (!k->nodes || k->nodes > LMB_CAL_NODES_MAX) return -1;
    for (uint32_t i = 0; i < k->nodes; i++) {
        STR(node_id[i]); STR(node_hardware_id[i]); STR(node_build_id[i]); STR(node_backend[i]);
        U32(layer_begin[i]); U32(layer_end[i]); U32(threads[i]);
        uint32_t disk; GET(lmb_cur_u32(&c, &disk));
        if (disk > 1) return -1;
        k->from_disk[i] = (uint8_t)disk;
    }
    GET(lmb_cal_get_double(&c, &r.decode_tok_s));
    GET(lmb_cal_get_double(&c, &r.ttft_seconds));
    GET(lmb_cal_get_double(&c, &r.measured_at));
    GET(lmb_cur_u32(&c, &r.samples));
    GET(lmb_cur_u32(&c, &r.prompt_tokens));
    GET(lmb_cur_u32(&c, &r.generated_tokens));
    if (c.off != c.len || !lmb_cal_valid(&r)) return -1;
    *out = r; return 0;
#undef GET
#undef STR
#undef U32
}

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
