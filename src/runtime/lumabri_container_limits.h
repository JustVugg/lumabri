/* Effective Linux cgroup budgets. Read-only: never changes controller policy.
 * Memory headroom includes all processes/cache charged to each visible parent,
 * not just this process. Missing observations are errors, not unlimited RAM.
 * Parsers accept streams so tests need no privileged cgroup manipulation.
 */
#ifndef LMB_CONTAINER_LIMITS_H
#define LMB_CONTAINER_LIMITS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

typedef struct {
    uint64_t memory_max, memory_available; /* UINT64_MAX: no visible limit */
    uint32_t cpu_threads; /* ceil(quota/period), not a throughput measurement */
} LmbContainerLimits;

static inline int lmb_cg_uint(const char *s, uint64_t *out) {
    if (!s || *s < '0' || *s > '9') return -1;
    uint64_t n = 0;
    do {
        unsigned d = (unsigned)(*s++ - '0');
        if (n > (UINT64_MAX - d) / 10) return -1;
        n = n * 10 + d;
    } while (*s >= '0' && *s <= '9');
    if (*s) return -1;
    *out = n; return 0;
}

static inline int lmb_cg_member(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *s = list; s && *s; ) {
        const char *end = strchr(s, ',');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (len == n && !memcmp(s, name, n)) return 1;
        s = end ? end + 1 : NULL;
    }
    return 0;
}

/* proc mountinfo escapes whitespace/backslashes with octal sequences. */
static inline int lmb_cg_path(const char *in, char out[PATH_MAX]) {
    size_t n = 0;
    while (*in) {
        unsigned char c = (unsigned char)*in++;
        if (c == '\\') {
            if (strlen(in) < 3 || in[0] < '0' || in[0] > '7' ||
                in[1] < '0' || in[1] > '7' || in[2] < '0' || in[2] > '7') return -1;
            unsigned v = (unsigned)(in[0]-'0')*64 + (unsigned)(in[1]-'0')*8 + (unsigned)(in[2]-'0');
            if (!v || v > 255) return -1;
            c = (unsigned char)v; in += 3;
        }
        if (!c || c < 32 || n + 1 >= PATH_MAX) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (out[0] != '/') return -1;
    /* No traversal outside the cgroup mount, including namespace-relative '..'. */
    for (char *p = out + 1; *p; ) {
        char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end-p) : strlen(p);
        if (!len || (len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.')) return -1;
        p = end ? end + 1 : p + len;
    }
    if (n > 1 && out[n-1] == '/') return -1;
    return 0;
}

static inline int lmb_cg_below(const char *path, const char *root) {
    size_t n = strlen(root);
    return !strcmp(root, "/") ||
        (!strncmp(path, root, n) && (!path[n] || path[n] == '/'));
}

/* 0: value, 1: controller file absent, -1: observation failed. */
static inline int lmb_cg_read(const char *dir, const char *file, char out[128]) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", dir, file);
    if (n < 0 || n >= (int)sizeof path) { errno = ENAMETOOLONG; return -1; }
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 1 : -1;
    size_t got = fread(out, 1, 127, f);
    int bad = ferror(f) || !got || got == 127;
    int saved = errno; fclose(f); errno = saved;
    if (bad) { errno = EIO; return -1; }
    out[got] = 0;
    if (memchr(out, 0, got)) { errno = EINVAL; return -1; }
    while (got && (out[got-1] == '\n' || out[got-1] == '\r' || out[got-1] == ' ')) out[--got] = 0;
    return 0;
}

static inline int lmb_cg_memory(const char *dir, int version, LmbContainerLimits *b) {
    char limit[128], usage[128];
    const char *keys[] = {version == 2 ? "memory.max" : "memory.limit_in_bytes", "memory.high"};
    for (unsigned i = 0; i < (version == 2 ? 2u : 1u); i++) {
        int rc = lmb_cg_read(dir, keys[i], limit);
        if (rc < 0) return -1;
        if (rc == 1 || (version == 2 && !strcmp(limit, "max"))) continue;
        uint64_t max, used;
        if (lmb_cg_uint(limit, &max)) { errno = EINVAL; return -1; }
        if (lmb_cg_read(dir, version == 2 ? "memory.current" : "memory.usage_in_bytes", usage) ||
            lmb_cg_uint(usage, &used)) { errno = EIO; return -1; }
        if (max < b->memory_max) b->memory_max = max;
        uint64_t available = used < max ? max - used : 0;
        if (available < b->memory_available) b->memory_available = available;
    }
    return 0;
}

static inline int lmb_cg_cpu(const char *dir, int version, LmbContainerLimits *b) {
    char quota[128], period[128];
    int rc = lmb_cg_read(dir, version == 2 ? "cpu.max" : "cpu.cfs_quota_us", quota);
    if (rc) return rc == 1 ? 0 : -1;
    const char *p = period;
    if (version == 2) {
        char *space = strchr(quota, ' ');
        if (!space) { errno = EINVAL; return -1; }
        *space = 0; p = space + 1;
    } else if (lmb_cg_read(dir, "cpu.cfs_period_us", period)) {
        errno = EIO; return -1;
    }
    uint64_t q, t;
    if (lmb_cg_uint(p, &t) || !t) { errno = EINVAL; return -1; }
    if (!strcmp(quota, version == 2 ? "max" : "-1")) return 0;
    if (lmb_cg_uint(quota, &q) || !q) { errno = EINVAL; return -1; }
    uint64_t threads = q/t + (q%t != 0);
    if (threads < b->cpu_threads) b->cpu_threads = (uint32_t)threads;
    return 0;
}

typedef struct {
    char group[PATH_MAX], mount[PATH_MAX], root[PATH_MAX];
    int version, memory, cpu;
} LmbCgroupPath;

static inline int lmb_cg_walk(const LmbCgroupPath *g, LmbContainerLimits *b) {
    char dir[PATH_MAX];
    const char *suffix = !strcmp(g->root, "/") ? g->group : g->group + strlen(g->root);
    int n = snprintf(dir, sizeof dir, "%s%s", !strcmp(g->mount, "/") ? "" : g->mount, suffix);
    if (n < 0 || n >= (int)sizeof dir) { errno = ENAMETOOLONG; return -1; }
    if (!dir[0]) strcpy(dir, "/");
    size_t len = strlen(dir);
    if (len > 1 && dir[len-1] == '/') dir[--len] = 0;
    for (;;) {
        struct stat st;
        if (stat(dir, &st) || !S_ISDIR(st.st_mode)) { errno = ENOENT; return -1; }
        if ((g->memory && lmb_cg_memory(dir, g->version, b)) ||
            (g->cpu && lmb_cg_cpu(dir, g->version, b))) return -1;
        if (!strcmp(dir, g->mount)) break;
        char *slash = strrchr(dir, '/');
        if (!slash) { errno = EINVAL; return -1; }
        if (slash == dir) dir[1] = 0; else *slash = 0;
        if (!lmb_cg_below(dir, g->mount)) { errno = EINVAL; return -1; }
    }
    return 0;
}

static inline int lmb_container_limits_read(FILE *membership, FILE *mounts, LmbContainerLimits *out) {
    if (!membership || !mounts || !out) { errno = EINVAL; return -1; }
    LmbCgroupPath groups[3] = {0};
    char line[16384];
    while (fgets(line, sizeof line, membership)) {
        if (!strchr(line, '\n') && !feof(membership)) { errno = EOVERFLOW; return -1; }
        line[strcspn(line, "\r\n")] = 0;
        char *a = strchr(line, ':'), *c = a ? strchr(a + 1, ':') : NULL;
        if (!a || !c) { errno = EINVAL; return -1; }
        *a = 0; *c = 0;
        for (unsigned i = 0; i < 3; i++) {
            int match = i == 0 ? (!strcmp(line, "0") && !a[1]) :
                lmb_cg_member(a+1, i == 1 ? "memory" : "cpu");
            if (!match) continue;
            if (groups[i].group[0] || lmb_cg_path(c+1, groups[i].group)) { errno = EINVAL; return -1; }
            groups[i].version = i == 0 ? 2 : 1;
            groups[i].memory = i != 2; groups[i].cpu = i != 1;
        }
    }
    if (ferror(membership)) return -1;
    while (fgets(line, sizeof line, mounts)) {
        if (!strchr(line, '\n') && !feof(mounts)) { errno = EOVERFLOW; return -1; }
        char *sep = strstr(line, " - ");
        if (!sep) { errno = EINVAL; return -1; }
        *sep = 0;
        char *save = NULL, *type = strtok_r(sep+3, " \n", &save);
        char *source = strtok_r(NULL, " \n", &save), *options = strtok_r(NULL, " \n", &save);
        if (!type || !source || !options) { errno = EINVAL; return -1; }
        if (strcmp(type, "cgroup") && strcmp(type, "cgroup2")) continue;
        char *parts[5], *first_save = NULL;
        for (unsigned i = 0; i < 5; i++) {
            parts[i] = strtok_r(i ? NULL : line, " ", &first_save);
            if (!parts[i]) { errno = EINVAL; return -1; }
        }
        char root[PATH_MAX], mount[PATH_MAX];
        if (lmb_cg_path(parts[3], root) || lmb_cg_path(parts[4], mount)) { errno = EINVAL; return -1; }
        for (unsigned i = 0; i < 3; i++) {
            LmbCgroupPath *g = &groups[i];
            if (!g->group[0] || !lmb_cg_below(g->group, root)) continue;
            if (i == 0 ? strcmp(type, "cgroup2") :
                (strcmp(type, "cgroup") || !lmb_cg_member(options, i == 1 ? "memory" : "cpu"))) continue;
            /* Prefer the mount exposing the most ancestors. */
            if (g->mount[0] && strlen(g->root) <= strlen(root)) continue;
            strcpy(g->mount, mount); strcpy(g->root, root);
        }
    }
    if (ferror(mounts)) return -1;
    LmbContainerLimits b = {UINT64_MAX, UINT64_MAX, UINT32_MAX};
    for (unsigned i = 0; i < 3; i++) {
        LmbCgroupPath *g = &groups[i];
        if (!g->group[0]) continue;
        if (!g->mount[0]) { errno = ENOENT; return -1; }
        if (lmb_cg_walk(g, &b)) return -1;
    }
    *out = b; return 0;
}

static inline int lmb_container_limits_probe(LmbContainerLimits *out) {
    *out = (LmbContainerLimits){UINT64_MAX, UINT64_MAX, UINT32_MAX};
#ifdef __linux__
    FILE *membership = fopen("/proc/self/cgroup", "r");
    if (!membership) return errno == ENOENT ? 0 : -1; /* kernels without cgroups */
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (!mounts) { int e = errno; fclose(membership); errno = e; return -1; }
    int rc = lmb_container_limits_read(membership, mounts, out), e = errno;
    fclose(mounts); fclose(membership); errno = e; return rc;
#else
    return 0;
#endif
}
#endif
