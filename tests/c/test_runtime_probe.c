#define _GNU_SOURCE
#include "lumabri_runtime_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--thread-capacity")) {
        const char *mode = getenv("LMB_TEST_CAPACITY");
        if (mode && !strcmp(mode, "hang")) { puts("1"); fflush(stdout); for (;;) pause(); }
        puts(mode ? mode : "1");
        return 0;
    }
    unsigned value = 99;
    assert(!lmb_runtime_thread_capacity(argv[0], &value) && value == 1);
    setenv("LMB_TEST_CAPACITY", "8", 1);
    assert(!lmb_runtime_thread_capacity(argv[0], &value) && value == 8);
    const char *bad[] = {"0", "257", "garbage", "8 trailing", "", "999999999999999999999999999999999999", "hang"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        setenv("LMB_TEST_CAPACITY", bad[i], 1);
        assert(lmb_runtime_thread_capacity(argv[0], &value) && value == 0);
    }
    assert(lmb_runtime_thread_capacity("/nonexistent/lumabri-test-runtime", &value));
    puts("runtime probe: valid, malformed, missing and hung executable PASS");
    return 0;
}
