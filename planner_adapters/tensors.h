/* Bounded safetensors inventory. Weight payloads are never read; small I64
 * layout buffers (e.g. PLE offsets) are metadata and are checked explicitly. */
#ifndef LUMABRI_PLAN_TENSORS_H
#define LUMABRI_PLAN_TENSORS_H
#include <dirent.h>
#include <sys/stat.h>

typedef struct {
    char name[512], dtype[32];
    uint64_t elements, bytes, shape[8];
    uint64_t meta_i64[64];
    unsigned rank;
} LmbPlanTensor;
typedef int (*LmbPlanTensorVisit)(const LmbPlanTensor *, void *);

static const char *lmb_plan_space(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}

/* Header-generated names are plain ASCII. Reject escaped names rather than
 * normalizing them differently from the runtime's tensor lookup. */
static const char *lmb_plan_string(const char *p, char *out, size_t cap) {
    if (*p++ != '"') return NULL;
    size_t n = 0;
    while (*p && *p != '"') {
        if ((unsigned char)*p < 32 || *p == '\\' || n + 1 >= cap) return NULL;
        out[n++] = *p++;
    }
    if (*p != '"') return NULL;
    out[n] = 0;
    return p + 1;
}

static const char *lmb_plan_uint(const char *p, uint64_t *n) {
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return NULL;
    uint64_t v = 0;
    do {
        if (v > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return NULL;
        v = v * 10 + (unsigned)(*p++ - '0');
    } while (*p >= '0' && *p <= '9');
    if (*p == 'e' || *p == 'E') return NULL;
    *n = v;
    return p;
}

static int lmb_plan_array(const char *p, uint64_t *values, unsigned cap, unsigned *count) {
    if (!p || *p++ != '[') return -1;
    *count = 0;
    p = lmb_plan_space(p);
    if (*p == ']') return 0;
    for (;;) {
        if (*count == cap || !(p = lmb_plan_uint(p, &values[(*count)++]))) return -1;
        p = lmb_plan_space(p);
        if (*p == ']') return 0;
        if (*p++ != ',') return -1;
        p = lmb_plan_space(p);
    }
}

/* Skip one balanced object, accepting escaped strings only in ignored
 * metadata. Tensor attributes are parsed strictly below. */
static const char *lmb_plan_object_end(const char *p) {
    if (*p != '{') return NULL;
    char stack[32]; unsigned depth = 0;
    int string = 0, escape = 0;
    do {
        unsigned char ch = (unsigned char)*p++;
        if (!ch) return NULL;
        if (string) {
            if (ch < 32) return NULL;
            if (escape) escape = 0;
            else if (ch == '\\') escape = 1;
            else if (ch == '"') string = 0;
        } else if (ch == '"') string = 1;
        else if (ch == '{' || ch == '[') {
            if (depth == sizeof stack) return NULL;
            stack[depth++] = ch == '{' ? '}' : ']';
        } else if (ch == '}' || ch == ']') {
            if (!depth || stack[--depth] != ch) return NULL;
        }
    } while (depth);
    return p;
}

static int lmb_plan_tensor_file(const char *path, uint64_t *header_budget,
                                LmbPlanTensorVisit visit, void *opaque) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat st;
    unsigned char prefix[8];
    uint64_t bytes = 0;
    int rc = -1;
    char *json = NULL;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size < 10 ||
        fread(prefix, 1, 8, f) != 8) goto done;
    for (unsigned i = 0; i < 8; i++) bytes |= (uint64_t)prefix[i] << (8 * i);
    if (bytes > *header_budget || bytes > (uint64_t)st.st_size - 8 || bytes < 2) goto done;
    *header_budget -= bytes;
    json = malloc((size_t)bytes + 1);
    if (!json || fread(json, 1, (size_t)bytes, f) != bytes) goto done;
    json[bytes] = 0;
    if (memchr(json, 0, (size_t)bytes)) goto done;
    const char *p = lmb_plan_space(json);
    if (*p++ != '{') goto done;
    unsigned tensors = 0;
    for (;;) {
        LmbPlanTensor t = {0};
        p = lmb_plan_space(p);
        if (*p == '}') { p++; break; }
        if (!(p = lmb_plan_string(p, t.name, sizeof t.name))) goto done;
        p = lmb_plan_space(p);
        if (*p++ != ':') goto done;
        p = lmb_plan_space(p);
        const char *end = lmb_plan_object_end(p);
        if (!end) goto done;
        if (strcmp(t.name, "__metadata__")) {
            uint64_t offsets[2]; unsigned noffsets;
            if (++tensors > 131072 ||
                lmb_json_string(p, "dtype", t.dtype, sizeof t.dtype) ||
                lmb_plan_array(lmb_json_member(p, "shape"), t.shape, 8, &t.rank) ||
                lmb_plan_array(lmb_json_member(p, "data_offsets"), offsets, 2, &noffsets) ||
                noffsets != 2 || offsets[0] > offsets[1] ||
                offsets[1] > (uint64_t)st.st_size - 8 - bytes) goto done;
            t.elements = 1;
            for (unsigned i = 0; i < t.rank; i++) t.elements = lmb_size_mul(t.elements, t.shape[i]);
            unsigned width = !strcmp(t.dtype, "F32") ? 4 :
                (!strcmp(t.dtype, "BF16") || !strcmp(t.dtype, "F16")) ? 2 :
                (!strcmp(t.dtype, "U8") || !strcmp(t.dtype, "I8")) ? 1 :
                !strcmp(t.dtype, "I64") ? 8 : 0;
            t.bytes = offsets[1] - offsets[0];
            if (!width || lmb_size_mul(t.elements, width) != t.bytes) goto done;
            if (width == 8) {
                unsigned char metadata[512];
                if (t.elements > 64 || t.bytes > *header_budget ||
                    fseeko(f, (off_t)(8 + bytes + offsets[0]), SEEK_SET) ||
                    fread(metadata, 1, (size_t)t.bytes, f) != t.bytes) goto done;
                *header_budget -= t.bytes;
                for (uint64_t i=0; i<t.elements; i++)
                    for (unsigned j=0; j<8; j++)
                        t.meta_i64[i] |= (uint64_t)metadata[i*8+j] << (8*j);
            }
            if (visit(&t, opaque)) goto done;
        }
        p = lmb_plan_space(end);
        if (*p == '}') { p++; break; }
        if (*p++ != ',') goto done;
        if (*lmb_plan_space(p) == '}') goto done;
    }
    if (*lmb_plan_space(p) || !tensors) goto done;
    rc = 0;
done:
    free(json); fclose(f); return rc;
}

static int lmb_plan_tensors(const char *root, LmbPlanTensorVisit visit, void *opaque) {
    DIR *dir = opendir(root);
    if (!dir) return -1;
    uint64_t budget = UINT64_C(64) << 20;
    unsigned files = 0;
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) { if (errno) rc = -1; break; }
        size_t n = strlen(e->d_name);
        if (n < 12 || strcmp(e->d_name + n - 12, ".safetensors")) continue;
        char path[1024];
        int len = snprintf(path, sizeof path, "%s/%s", root, e->d_name);
        if (++files > 512 || len < 0 || (size_t)len >= sizeof path ||
            lmb_plan_tensor_file(path, &budget, visit, opaque)) { rc = -1; break; }
    }
    closedir(dir);
    return files ? rc : -1;
}
#endif
