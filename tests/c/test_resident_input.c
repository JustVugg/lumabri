#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    int remote = open(argv[1], O_RDONLY), source = open(argv[2], O_RDONLY);
    assert(remote >= 0 && source >= 0);
    char got[8191], expected[8191];
    off_t at = 0;
    for (;;) {
        ssize_t want = pread(source, expected, sizeof expected, at);
        assert(want >= 0);
        ssize_t have = pread(remote, got, sizeof got, at);
        assert(have == want && !memcmp(got, expected, (size_t)want));
        if (!want) break;
        at += want;
    }
    /* gate/up/down interleaving across three verified blocks must reuse RAM,
     * not refetch an 8-MiB block for every tiny matrix row. */
    for (int round = 0; round < 32; round++) {
        for (int block = 0; block < 3; block++) {
            off_t pos = (off_t)block * 1024 * 1024;
            assert(pread(source, expected, 512, pos) == 512);
            assert(pread(remote, got, 512, pos) == 512);
            assert(!memcmp(got, expected, 512));
        }
    }
    /* Keys include the shard, and bounded eviction still returns exact bytes. */
    char other_path[PATH_MAX], other_source[PATH_MAX];
    assert(snprintf(other_path, sizeof other_path, "%s.other.safetensors", argv[1]) < (int)sizeof other_path);
    assert(snprintf(other_source, sizeof other_source, "%s.other.safetensors", argv[2]) < (int)sizeof other_source);
    int other = open(other_path, O_RDONLY), original = open(other_source, O_RDONLY);
    assert(other >= 0 && original >= 0);
    for (int block = 0; block < 3; block++) {
        off_t pos = (off_t)block * 1024 * 1024;
        assert(pread(original, expected, 512, pos) == 512);
        assert(pread(other, got, 512, pos) == 512);
        assert(!memcmp(got, expected, 512));
    }
    for (int block = 0; block < 3; block++) {
        off_t pos = (off_t)block * 1024 * 1024;
        assert(pread(source, expected, 512, pos) == 512);
        assert(pread(remote, got, 512, pos) == 512);
        assert(!memcmp(got, expected, 512));
    }
    close(other); close(original);
    int (*seal)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "lmb_weights_seal");
    int (*retain)(int, uint64_t, uint64_t) =
        (int (*)(int, uint64_t, uint64_t))dlsym(RTLD_DEFAULT, "lmb_weights_retain");
    assert(retain && !retain(remote, 4000, sizeof got));
    assert(seal && !seal());
    assert(!seal()); /* idempotent cleanup; cached input must not survive seal */
    assert(pread(source, expected, sizeof expected, 4000) == sizeof expected);
    assert(pread(remote, got, sizeof got, 4000) == sizeof got);
    assert(!memcmp(got, expected, sizeof got));
    assert(retain(remote, 0, 1) == -1);
    errno = 0; assert(pread(remote, got, 1, 0) == -1 && errno == EPERM);
    assert(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, remote, 0) == MAP_FAILED);
    assert(!fopen(argv[1], "rb"));
    close(remote); close(source);
    puts("RESIDENT INPUT PASS: verified RAM transfer, retained tensor reads, sealed external reads, no file-backed mappings");
    return 0;
}
