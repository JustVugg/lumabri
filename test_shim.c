/* test_shim.c — correctness client for liblumabri.so.
 *
 * Exercises exactly the libc surface the engines use on a model directory
 * (opendir/readdir, open+fstat+pread, fopen+fread) against the virtual root,
 * and compares every byte with the source directory the maintainers serve.
 *
 *   LD_PRELOAD=./liblumabri.so LUMABRI_VROOT=... LUMABRI_CACHE=... \
 *     ./test_shim VROOT SRCDIR
 *
 * Exit 0 = every comparison passed. Any mismatch or unexpected errno is
 * fatal and named — the whole point of lumabri is that the network may only
 * change where bytes come from, never which bytes.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (1u << 20)

static const char *g_vroot, *g_src;
static int g_files;

static void die(const char *what, const char *path) {
    fprintf(stderr, "FAIL: %s (%s): %s\n", what, path, strerror(errno));
    exit(1);
}

static void compare_file(const char *rel) {
    char vpath[1024], spath[1024];
    snprintf(vpath, sizeof vpath, "%s/%s", g_vroot, rel);
    snprintf(spath, sizeof spath, "%s/%s", g_src, rel);

    int vfd = open(vpath, O_RDONLY);
    int sfd = open(spath, O_RDONLY);
    if (vfd < 0) die("open virtual", vpath);
    if (sfd < 0) die("open source", spath);

    struct stat vst, sst;
    if (fstat(vfd, &vst) || fstat(sfd, &sst)) die("fstat", rel);
    if (vst.st_size != sst.st_size) {
        fprintf(stderr, "FAIL: size mismatch on %s: %lld vs %lld\n",
                rel, (long long)vst.st_size, (long long)sst.st_size);
        exit(1);
    }

    static uint8_t vb[CHUNK], sb[CHUNK];
    for (off_t off = 0; off < sst.st_size; off += CHUNK) {
        ssize_t vn = pread(vfd, vb, CHUNK, off);
        ssize_t sn = pread(sfd, sb, CHUNK, off);
        if (vn < 0) die("pread virtual", rel);
        if (vn != sn || memcmp(vb, sb, (size_t)sn)) {
            fprintf(stderr, "FAIL: bytes differ in %s at offset %lld\n",
                    rel, (long long)off);
            exit(1);
        }
    }
    /* unaligned slices crossing block boundaries */
    for (int i = 1; i <= 4 && sst.st_size > 3; i++) {
        off_t off = (sst.st_size * i) / 5 - 1;
        size_t n = (size_t)(sst.st_size - off < 8191 ? sst.st_size - off : 8191);
        ssize_t vn = pread(vfd, vb, n, off);
        ssize_t sn = pread(sfd, sb, n, off);
        if (vn != sn || vn < 0 || memcmp(vb, sb, (size_t)vn)) {
            fprintf(stderr, "FAIL: slice differs in %s at offset %lld\n",
                    rel, (long long)off);
            exit(1);
        }
    }
    close(vfd); close(sfd);

    /* the stdio path the engines use for config/tokenizer */
    if (sst.st_size < (10 << 20)) {
        FILE *vf = fopen(vpath, "rb");
        FILE *sf = fopen(spath, "rb");
        if (!vf || !sf) die("fopen", rel);
        size_t vn, sn;
        do {
            vn = fread(vb, 1, CHUNK, vf);
            sn = fread(sb, 1, CHUNK, sf);
            if (vn != sn || memcmp(vb, sb, vn)) {
                fprintf(stderr, "FAIL: fread differs in %s\n", rel);
                exit(1);
            }
        } while (vn == CHUNK);
        fclose(vf); fclose(sf);
    }
    g_files++;
}

static void walk(const char *rel) {
    char vpath[1024];
    if (rel[0]) snprintf(vpath, sizeof vpath, "%s/%s", g_vroot, rel);
    else        snprintf(vpath, sizeof vpath, "%s", g_vroot);
    DIR *d = opendir(vpath);
    if (!d) die("opendir", vpath);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, "manifest.txt")) continue;   /* lumabri's own */
        char sub[1024], spath[1024];
        if (rel[0]) snprintf(sub, sizeof sub, "%s/%s", rel, e->d_name);
        else        snprintf(sub, sizeof sub, "%s", e->d_name);
        snprintf(spath, sizeof spath, "%s/%s", g_src, sub);
        struct stat st;
        if (stat(spath, &st)) continue;         /* chatter-local extras */
        if (S_ISDIR(st.st_mode)) walk(sub);
        else if (S_ISREG(st.st_mode)) compare_file(sub);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s VROOT SRCDIR\n", argv[0]);
        return 2;
    }
    g_vroot = argv[1]; g_src = argv[2];

    walk("");

    /* writing model bytes must be refused — EROFS, never silent corruption */
    char first[1024] = "";
    DIR *d = opendir(g_src);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[1024];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", g_src, e->d_name);
        if (!stat(p, &st) && S_ISREG(st.st_mode)) {
            snprintf(first, sizeof first, "%s/%s", g_vroot, e->d_name);
            break;
        }
    }
    if (d) closedir(d);
    if (first[0]) {
        errno = 0;
        if (open(first, O_WRONLY) != -1 || errno != EROFS) {
            fprintf(stderr, "FAIL: writing a model file was not refused with EROFS\n");
            return 1;
        }
    }

    /* chatter-local state beside the model (the engines write .coli_usage
     * into the snap dir) must work */
    char note[1024];
    snprintf(note, sizeof note, "%s/.coli_usage_selftest", g_vroot);
    FILE *nf = fopen(note, "w");
    if (!nf || fputs("lumabri\n", nf) == EOF) {
        fprintf(stderr, "FAIL: chatter-local write beside the model failed\n");
        return 1;
    }
    fclose(nf);

    printf("test_shim: PASS (%d files byte-identical)\n", g_files);
    return 0;
}
