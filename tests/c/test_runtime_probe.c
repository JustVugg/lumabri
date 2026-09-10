#define _GNU_SOURCE
#include "lumabri_runtime_probe.h"
#include "src/runtime/lumabri_backend_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    uint64_t backend = 999;
    assert(lmb_backend_request("cpu", NULL));
    assert(!lmb_backend_request(NULL, &backend) && !backend);
    assert(!lmb_backend_request("auto", &backend) && !backend);
    assert(!lmb_backend_request("cpu", &backend) && backend == LMB_SEG_CAP_CPU);
    assert(lmb_backend_matches(backend, LMB_SEG_CAP_CPU | LMB_SEG_CAP_SNAPSHOT));
    assert(!lmb_backend_matches(backend, 0));
    assert(!lmb_backend_matches(backend, LMB_SEG_CAP_CUDA));
    assert(!lmb_backend_matches(backend, LMB_SEG_CAP_CPU | LMB_SEG_CAP_CUDA));
    assert(!lmb_backend_matches(LMB_SEG_CAP_CUDA, LMB_SEG_CAP_CUDA));
    assert(lmb_backend_matches(0, LMB_SEG_CAP_CUDA));
    const char *bad_backend[] = {"", "CPU", "cuda", "metal", "hip", "vulkan", "cpu ", "cpu,cuda"};
    for (unsigned i = 0; i < sizeof bad_backend / sizeof *bad_backend; i++) {
        backend = 999;
        assert(lmb_backend_request(bad_backend[i], &backend) && !backend);
    }
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
