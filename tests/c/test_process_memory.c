#define _GNU_SOURCE
#include "src/runtime/lumabri_resident_guard.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

static int parse(const char *s, int rollup, LmbProcessMemory *m) {
    FILE *f = tmpfile();
    assert(f && fputs(s, f) >= 0);
    rewind(f);
    int rc = lmb_process_memory_parse(f, rollup, m);
    fclose(f);
    return rc;
}

static void test_parser(void) {
    LmbProcessMemory m = {0};
    assert(!parse("Name: engine\nVmRSS: 4096 kB\nVmSwap: 512 kB\nVmLck: 128 kB\n", 0, &m));
    assert(m.known == (LMB_PM_RSS|LMB_PM_SWAP|LMB_PM_LOCKED));
    assert(m.rss == 4096*1024 && m.swap == 512*1024 && m.locked == 128*1024);
    assert(lmb_process_memory_charge(&m) == (4096+512)*1024);
    assert(!parse("Rss: 7 kB\nSwap: 0 kB\nLocked: 0 kB\n", 1, &m));
    assert(m.rss == 7*1024 && !m.swap && !m.locked);
    assert(!parse("VmRSS: 4096 kB\n", 0, &m));
    assert(m.known == LMB_PM_RSS); /* absent is not zero */
    const char *bad[] = {"VmSwap: 0 kB\n", "VmRSS: -1 kB\n", "VmRSS: +1 kB\n",
        "VmRSS: 1 MB\n", "VmRSS: 1 kB junk\n", "VmRSS: 1kB\n",
        "VmRSS: 18446744073709551616 kB\n", "VmRSS: 18446744073709551615 kB\n",
        "VmRSS: 1 kB\nVmSwap: 0 kB\nVmSwap: 1 kB\n"};
    for (size_t i = 0; i < sizeof bad/sizeof *bad; i++) {
        LmbProcessMemory before = m;
        assert(parse(bad[i], 0, &m) == -1);
        assert(!memcmp(&m, &before, sizeof m));
    }
    char long_line[1100]; memset(long_line, 'x', sizeof long_line-1);
    long_line[sizeof long_line-1] = 0;
    assert(parse(long_line, 0, &m) == -1);
}

static void test_charge_and_guard(void) {
    LmbProcessMemory m = {.known=LMB_PM_RSS|LMB_PM_SWAP, .rss=100};
    LmbResidentGuard g = {.enabled=1};
    assert(!lmb_resident_guard_observe(&g, &m));
    m.swap = 25;
    assert(lmb_process_memory_charge(&m) == 125);
    assert(lmb_resident_guard_observe(&g, &m) == -1);
    assert(atomic_load(&g.fault) == LMB_RESIDENT_PAGED);
    m.swap = 0;
    assert(lmb_resident_guard_observe(&g, &m) == -1); /* never silently recovered */
    m.known = LMB_PM_RSS|LMB_PM_COMPRESSED|LMB_PM_FOOTPRINT;
    m.compressed = 30; m.footprint = 200;
    assert(lmb_process_memory_charge(&m) == 200);
    assert(lmb_resident_memory_reason(&m) == LMB_RESIDENT_COMPRESSED);
    m.footprint = 20;
    assert(lmb_process_memory_charge(&m) == 130);
    m.rss = UINT64_MAX-10;
    assert(lmb_process_memory_charge(&m) == UINT64_MAX);
    m.known = LMB_PM_RSS;
    assert(lmb_resident_memory_reason(&m) == LMB_RESIDENT_UNKNOWN);
    m.known = 0;
    assert(lmb_process_memory_charge(&m) == UINT64_MAX);
    LmbResidentGuard unknown = {.enabled=1}, disabled = {0};
    assert(lmb_resident_guard_observe(&unknown, &m) == -1);
    assert(atomic_load(&unknown.fault) == LMB_RESIDENT_UNKNOWN);
    assert(!lmb_resident_guard_observe(&disabled, &m));
}

static void test_json_and_native_probe(void) {
    LmbProcessMemory m = {.known=LMB_PM_RSS, .rss=1024};
    FILE *f = tmpfile(); assert(f);
    lmb_process_memory_json(f, &m); rewind(f);
    char json[1024]; assert(fgets(json, sizeof json, f)); fclose(f);
    assert(strstr(json, "\"rss_bytes\":1024") && strstr(json, "\"swap_bytes\":null"));
    assert(!lmb_process_memory_probe(&m, 0) && (m.known & LMB_PM_RSS));
    assert(!lmb_process_memory_probe(&m, 1) && (m.known & LMB_PM_RSS));
#ifdef __APPLE__
    assert((m.known & LMB_PM_COMPRESSED) && !(m.known & LMB_PM_SWAP));
#else
    assert((m.known & LMB_PM_SWAP) && (m.known & LMB_PM_LOCKED));
#endif
    lmb_process_memory_json(stdout, &m); putchar('\n');
}

static void test_policy(void) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        LmbResidentGuard invalid = {0};
        assert(!setenv("LUMABRI_RESIDENCY_POLICY", "silently-ignore", 1));
        assert(lmb_resident_guard_begin(&invalid) == -1);
        /* Restrict only this disposable child. Never alter host swap, limits
         * of a running model, or permissions to make a test pass. */
        struct rlimit limit = {0,0};
        assert(!setrlimit(RLIMIT_MEMLOCK, &limit));
        assert(!setenv("LUMABRI_RESIDENCY_POLICY", "locked", 1));
        LmbResidentGuard locked = {0};
        assert(lmb_resident_guard_begin(&locked) == -1);
        assert(!locked.locked);
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_native_pressure(void) {
    /* Bounded to this child's own 4 MiB. Never create host-wide pressure or
     * change swap settings. Some kernels/CI machines have no usable swap;
     * that is reported as unavailable, not called a pressure-test success. */
#if defined(__linux__) && defined(MADV_PAGEOUT) && !defined(LMB_RESIDENT_ASAN)
    fflush(NULL);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        size_t n = 4u*1024*1024;
        void *p = mmap(NULL, n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        assert(p != MAP_FAILED);
        memset(p, 0x5a, n);
        LmbResidentGuard g = {.enabled=1};
        LmbProcessMemory m = {0};
        assert(!lmb_process_memory_probe(&m, 1));
        if (m.swap) { puts("PAGEOUT: unavailable (child already paged)"); fflush(stdout); _exit(0); }
        int advised = madvise(p, n, MADV_PAGEOUT);
        for (int i = 0; !advised && i < 10; i++) {
            assert(!lmb_process_memory_probe(&m, 1));
            if (m.swap) break;
            usleep(20000);
        }
        if (!advised && m.swap) {
            assert(lmb_resident_guard_check(&g) == -1);
            assert(atomic_load(&g.fault) == LMB_RESIDENT_PAGED);
            assert(lmb_process_memory_charge(&m) >= m.swap);
            puts("PAGEOUT: PASS (kernel evicted private test pages; inference guard refused)");
        } else puts("PAGEOUT: unavailable (kernel did not evict test pages)");
        assert(!munmap(p, n)); fflush(stdout); _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

static void test_native_lock(void) {
#ifndef LMB_RESIDENT_ASAN
    fflush(NULL);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        assert(!setenv("LUMABRI_RESIDENCY_POLICY", "locked", 1));
        LmbResidentGuard g = {0};
        if (lmb_resident_guard_begin(&g)) {
            puts("LOCK: unavailable under this process's OS allowance"); fflush(stdout); _exit(0);
        }
        assert(g.locked);
        LmbProcessMemory m = {0}; assert(!lmb_process_memory_probe(&m, 1));
        if (m.known & LMB_PM_LOCKED) assert(m.locked > 0);
        assert(!lmb_resident_guard_check(&g));
        assert(!munlockall());
        puts("LOCK: PASS (OS accepted current/future process locking)"); fflush(stdout); _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

int main(void) {
    test_parser(); test_charge_and_guard(); test_json_and_native_probe(); test_policy();
    test_native_pressure(); test_native_lock();
    puts("PROCESS MEMORY: PASS (unknowns, conservative charge, latched pressure, OS lock denial)");
    return 0;
}
