/* Run in a disposable native Linux scope with MemoryMax=512M and
 * CPUQuota=100%. Unlike parser fixtures this checks the kernel's counters,
 * the public machine profile and the governor against a real allocation. */
#define _GNU_SOURCE
#include "lumabri_machine.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    char home[] = "/tmp/lmb-budget-probe-XXXXXX";
    assert(mkdtemp(home)); assert(!setenv("HOME", home, 1));
    assert(!unsetenv("LUMABRI_GOVERNOR_FILE"));
    setenv("LUMABRI_NO_DISK_PROBE", "1", 1);
    LmbMachineProfile before, after;
    assert(!lmb_machine_probe(&before, ".", NULL));
    assert(before.ram_total_bytes == (512ull << 20));
    assert(before.ram_available_bytes > (64ull << 20));
    assert(before.ram_available_bytes <= before.ram_total_bytes);
    assert(before.logical_cpus == 1 && before.physical_cores == 1);
    size_t n = 32u << 20;
    volatile unsigned char *p = malloc(n); assert(p);
    for (size_t i = 0; i < n; i += 4096) p[i] = 1;
    struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL);
    after = before; lmb_machine_refresh_resources(&after, ".");
    assert(after.ram_available_bytes + (24ull << 20) < before.ram_available_bytes);
    /* A reserve larger than the container must stop admission even while the
     * host still has plenty of RAM. No persistent pause state is modified. */
    LmbGovernor gov; lmb_governor_init(&gov, 2ull << 30);
    assert(lmb_governor_poll(&gov) == LMB_GOV_PAUSED);
    assert(lmb_governor_reason(&gov) == LMB_GOV_REASON_RAM_CRITICAL);
    free((void *)p);
    assert(!rmdir(home));
    lmb_machine_print(stdout, &after, 1);
    puts("NATIVE CONTAINER BUDGET: PASS (512 MiB, 1 CPU, live headroom, admission)");
    return 0;
}
