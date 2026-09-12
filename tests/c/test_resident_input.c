#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
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
    int (*seal)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "lmb_weights_seal");
    int (*retain)(int, uint64_t, uint64_t) =
        (int (*)(int, uint64_t, uint64_t))dlsym(RTLD_DEFAULT, "lmb_weights_retain");
    assert(retain && !retain(remote, 4000, sizeof got));
    assert(seal && !seal());
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
