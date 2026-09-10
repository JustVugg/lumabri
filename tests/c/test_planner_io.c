/* Unit fault injection and native-shim client for bounded tensor inspection. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int faults, calls;
static uint64_t read_bytes;
static ssize_t observed_pread(int fd, void *buf, size_t n, off_t off) {
    calls++;
    if (faults == 1 && calls == 1) { errno = EINTR; return -1; }
    if (faults == 2) return 0;
    if (faults == 3) { errno = EIO; return -1; }
    if (faults == 1 && n > 3) n = 3;
    ssize_t got = pread(fd, buf, n, off);
    if (got > 0) read_bytes += (uint64_t)got;
    return got;
}
#define pread observed_pread
#include "lumabri_planner.h"
#undef pread

static int visited;
static int visit(const LmbPlanTensor *t, void *opaque) {
    (void)opaque;
    if (!strcmp(t->name, "weights")) {
        assert(!strcmp(t->dtype, "U8"));
    } else {
        assert(!strcmp(t->name, "layout"));
        assert(t->elements == 2 && t->bytes == 16);
        assert(t->meta_i64[0] == 17 && t->meta_i64[1] == 29);
    }
    visited++;
    return 0;
}

static int inspect(const char *path, int mode) {
    uint64_t budget = 65536;
    faults = mode; calls = visited = 0; read_bytes = 0;
    return lmb_plan_tensor_file_id(path, &budget, visit, NULL, 0);
}

int main(int argc, char **argv) {
    if (argc == 2) {
        assert(!inspect(argv[1], 0));
        assert(visited == 2 && read_bytes < 65536);
        printf("PLANNER IO: PASS (%llu requested bytes, two tensors, layout values verified)\n",
               (unsigned long long)read_bytes);
        return 0;
    }
    assert(argc == 1);
    char path[] = "/tmp/lumabri-planner-io-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    const char *header = "{\"weights\":{\"dtype\":\"U8\",\"shape\":[512],\"data_offsets\":[0,512]},"
        "\"layout\":{\"dtype\":\"I64\",\"shape\":[2],\"data_offsets\":[512,528]}}";
    uint64_t n = strlen(header);
    unsigned char prefix[8], layout[16] = {17,0,0,0,0,0,0,0,29};
    for (unsigned i=0; i<8; i++) prefix[i] = (unsigned char)(n >> (8*i));
    assert(write(fd, prefix, 8) == 8);
    assert(write(fd, header, (size_t)n) == (ssize_t)n);
    assert(lseek(fd, 512, SEEK_CUR) >= 0);
    assert(write(fd, layout, 16) == 16);
    assert(!inspect(path, 0) && visited == 2 && read_bytes == 8+n+16);
    assert(!inspect(path, 1) && visited == 2 && read_bytes == 8+n+16);
    assert(inspect(path, 2) == -1 && visited == 0); /* EOF is not zero-filled data */
    assert(inspect(path, 3) == -1 && visited == 0); /* transport failure */
    faults = 0;
    assert(lmb_plan_read_at(fd, prefix, 1, UINT64_MAX) == -1);
    uint64_t budget = n-1;
    assert(lmb_plan_tensor_file_id(path, &budget, visit, NULL, 0) == -1);
    assert(!ftruncate(fd, (off_t)(8+n+520)));
    assert(inspect(path, 0) == -1); /* declared layout outside truncated file */
    close(fd); assert(!unlink(path));
    puts("PLANNER IO: PASS (bounded reads, short reads, EINTR, EOF, EIO, offsets, budget, truncation)");
    return 0;
}
