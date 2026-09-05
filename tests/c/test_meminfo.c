#include "lumabri_machine.h"

#include <assert.h>
#include <stdio.h>

static void parse(const char *text, uint64_t *total, uint64_t *available) {
    FILE *stream = tmpfile();
    assert(stream);
    assert(fputs(text, stream) >= 0);
    rewind(stream);
    assert(lmb_machine_read_meminfo(stream, total, available, NULL, NULL) == 0);
    fclose(stream);
}

int main(void) {
    uint64_t total = 0, available = 0;

    /* A modern Linux value remains authoritative. The fallback inputs are
     * deliberately much larger so accidentally combining them fails. */
    parse("MemTotal: 100000 kB\n"
          "MemAvailable: 12000 kB\n"
          "MemFree: 90000 kB\n"
          "Buffers: 1000 kB\n"
          "Cached: 2000 kB\n"
          "SReclaimable: 3000 kB\n"
          "Shmem: 500 kB\n", &total, &available);
    assert(total == 100000ull * 1024);
    assert(available == 12000ull * 1024);

    /* WSL1 omits MemAvailable. Use free + buffers + reclaimable cache -
     * shared memory, which is the value old procps reports as available. */
    parse("MemTotal: 134217728 kB\n"
          "MemFree: 83886080 kB\n"
          "Buffers: 100000 kB\n"
          "Cached: 200000 kB\n"
          "SReclaimable: 50000 kB\n"
          "Shmem: 25000 kB\n", &total, &available);
    assert(available == 84211080ull * 1024);

    /* Malformed legacy counters can never publish more RAM than exists. */
    parse("MemTotal: 1000 kB\n"
          "MemFree: 900 kB\n"
          "Cached: 900 kB\n", &total, &available);
    assert(available == total);

    puts("MEMINFO TEST: PASS (native MemAvailable preserved; WSL1 fallback)");
    return 0;
}
