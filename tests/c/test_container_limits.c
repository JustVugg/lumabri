#define _GNU_SOURCE
#include "src/runtime/lumabri_container_limits.h"
#include <assert.h>
#include <unistd.h>

static char temp[PATH_MAX], paths[64][PATH_MAX];
static unsigned npaths;
static void remember(const char *path) {
    for (unsigned i = 0; i < npaths; i++) if (!strcmp(paths[i], path)) return;
    assert(npaths < 64); strcpy(paths[npaths++], path);
}
static void path_for(char out[PATH_MAX], const char *name) {
    int n = snprintf(out, PATH_MAX, "%s/%s", temp, name);
    assert(n > 0 && n < PATH_MAX);
}
static void directory(const char *name) {
    char p[PATH_MAX]; path_for(p, name); assert(!mkdir(p, 0700)); remember(p);
}
static void value(const char *name, const char *text) {
    char p[PATH_MAX]; path_for(p, name); FILE *f = fopen(p, "w");
    assert(f && fputs(text, f) >= 0); assert(!fclose(f)); remember(p);
}
static FILE *stream(const char *text) {
    FILE *f = tmpfile(); assert(f); assert(fputs(text, f) >= 0); rewind(f); return f;
}
static int read_limits(const char *membership, const char *mounts, LmbContainerLimits *out) {
    FILE *c = stream(membership), *m = stream(mounts);
    int rc = lmb_container_limits_read(c, m, out);
    fclose(c); fclose(m); return rc;
}
static void mount_line(char out[16384], const char *root, const char *name, const char *type, const char *opts) {
    char p[PATH_MAX], escaped[PATH_MAX*4]; path_for(p, name);
    size_t n = 0;
    for (const char *s = p; *s; s++) {
        if (*s == ' ') { memcpy(escaped+n, "\\040", 4); n += 4; }
        else escaped[n++] = *s;
    }
    escaped[n] = 0;
    int rc = snprintf(out, 16384, "10 1 0:42 %s %s ro,nosuid - %s cgroup rw,%s\n", root, escaped, type, opts);
    assert(rc > 0 && rc < 16384);
}

int main(void) {
    snprintf(temp, sizeof temp, "/tmp/lmb-cgroup-test-XXXXXX"); assert(mkdtemp(temp));
    directory("unified hierarchy"); directory("unified hierarchy/pods"); directory("unified hierarchy/pods/test");
    char mounts[16384]; mount_line(mounts, "/", "unified hierarchy", "cgroup2", "");
    value("unified hierarchy/pods/memory.max", "8589934592\n");
    value("unified hierarchy/pods/memory.current", "7516192768\n");
    value("unified hierarchy/pods/cpu.max", "150000 100000\n");
    value("unified hierarchy/pods/test/memory.max", "4294967296\n");
    value("unified hierarchy/pods/test/memory.current", "1073741824\n");
    value("unified hierarchy/pods/test/cpu.max", "400000 100000\n");
    LmbContainerLimits b;
    assert(!read_limits("0::/pods/test\n", mounts, &b));
    assert(b.memory_max == (4ull<<30));
    assert(b.memory_available == (1ull<<30)); /* parent's siblings consume 6 GiB */
    assert(b.cpu_threads == 2);

    /* A dynamically lowered memory.high is a preparation/admission boundary. */
    value("unified hierarchy/pods/memory.high", "8053063680\n");
    assert(!read_limits("0::/pods/test\n", mounts, &b));
    assert(b.memory_available == (512ull<<20));
    value("unified hierarchy/pods/memory.current", "8589934592\n");
    assert(!read_limits("0::/pods/test\n", mounts, &b));
    assert(!b.memory_available);
    value("unified hierarchy/pods/memory.current", "7516192768\n");
    value("unified hierarchy/pods/memory.high", "max\n");
    value("unified hierarchy/pods/test/cpu.max", "50000 100000\n");
    assert(!read_limits("0::/pods/test\n", mounts, &b) && b.cpu_threads == 1);

    /* Mount of a subtree: membership and mount root are in the same namespace. */
    mount_line(mounts, "/pods", "unified hierarchy/pods", "cgroup2", "");
    assert(!read_limits("0::/pods/test\n", mounts, &b));
    assert(b.memory_available == (1ull<<30));
    /* Namespace root can itself have limits; do not skip that directory. */
    mount_line(mounts, "/", "unified hierarchy/pods/test", "cgroup2", "");
    assert(!read_limits("0::/\n", mounts, &b));
    assert(b.memory_available == (3ull<<30));

    value("unified hierarchy/pods/test/memory.max", "-1\n");
    assert(read_limits("0::/\n", mounts, &b) == -1);
    value("unified hierarchy/pods/test/memory.max", "18446744073709551616\n");
    assert(read_limits("0::/\n", mounts, &b) == -1);
    value("unified hierarchy/pods/test/memory.max", "4294967296\n");
    char p[PATH_MAX]; path_for(p, "unified hierarchy/pods/test/memory.current");
    assert(!unlink(p)); assert(read_limits("0::/\n", mounts, &b) == -1);
    value("unified hierarchy/pods/test/memory.current", "1073741824\n");
    if (getuid()) {
        assert(!chmod(p, 0000)); assert(read_limits("0::/\n", mounts, &b) == -1);
        assert(!chmod(p, 0600));
    }
    assert(read_limits("0::/../test\n", mounts, &b) == -1);
    assert(read_limits("0::/\n0::/\n", mounts, &b) == -1);
    assert(read_limits("0::/\n", "", &b) == -1);
    value("unified hierarchy/pods/test/cpu.max", "50000 0\n");
    assert(read_limits("0::/\n", mounts, &b) == -1);
    value("unified hierarchy/pods/test/cpu.max", "max 100000\n");
    value("unified hierarchy/pods/test/memory.max", "max\n");
    assert(!read_limits("0::/\n", mounts, &b));
    assert(b.memory_max == UINT64_MAX && b.memory_available == UINT64_MAX && b.cpu_threads == UINT32_MAX);

    directory("v1memory"); directory("v1memory/container");
    directory("v1cpu"); directory("v1cpu/container");
    value("v1memory/memory.limit_in_bytes", "9223372036854771712\n");
    value("v1memory/memory.usage_in_bytes", "1073741824\n");
    value("v1memory/container/memory.limit_in_bytes", "2147483648\n");
    value("v1memory/container/memory.usage_in_bytes", "536870912\n");
    value("v1cpu/container/cpu.cfs_quota_us", "200000\n");
    value("v1cpu/container/cpu.cfs_period_us", "100000\n");
    value("v1cpu/cpu.cfs_quota_us", "-1\n");
    value("v1cpu/cpu.cfs_period_us", "100000\n");
    char m1[16384], m2[16384], both[32768];
    mount_line(m1, "/", "v1memory", "cgroup", "memory");
    mount_line(m2, "/", "v1cpu", "cgroup", "cpu,cpuacct");
    snprintf(both, sizeof both, "%s%s", m1, m2);
    assert(!read_limits("5:memory:/container\n6:cpu,cpuacct:/container\n7:name=systemd:/\n", both, &b));
    assert(b.memory_max == (2ull<<30) && b.memory_available == (1536ull<<20) && b.cpu_threads == 2);
    assert(read_limits("6:cpu:/container\n", m1, &b) == -1);
    assert(!read_limits("", "", &b) && b.memory_available == UINT64_MAX);

    for (unsigned i = npaths; i; i--)
        if (unlink(paths[i-1])) assert(!rmdir(paths[i-1]));
    assert(!rmdir(temp));
    assert(!lmb_container_limits_probe(&b));
    printf("CONTAINER LIMITS: PASS (v1/v2, parents, namespaces, zero headroom, unknown observations)\n");
    return 0;
}
