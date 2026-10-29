/* Read-only checkpoint metadata access. Preserve the POSIX descriptor path:
 * the CAS shim fetches bounded pread ranges, not entire safetensors files. */
#ifndef LMB_PLAN_FILES_H
#define LMB_PLAN_FILES_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>

static inline wchar_t *lmb_plan_wide(const char *path) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!count) { errno = EINVAL; return NULL; }
    wchar_t *wide = malloc((size_t)count * sizeof *wide);
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide); errno = EINVAL; return NULL;
    }
    return wide;
}
#endif

static inline int lmb_plan_open(const char *path) {
#ifdef _WIN32
    wchar_t *wide = lmb_plan_wide(path); if (!wide) return -1;
    int fd = _wopen(wide, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    free(wide); return fd;
#else
    return open(path, O_RDONLY | O_CLOEXEC);
#endif
}

static inline int lmb_plan_file_size(int fd, uint64_t *size) {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) || (st.st_mode & _S_IFMT) != _S_IFREG || st.st_size < 0) return -1;
#else
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) return -1;
#endif
    *size = (uint64_t)st.st_size; return 0;
}

static inline FILE *lmb_plan_fopen(const char *path) {
#ifdef _WIN32
    int fd = lmb_plan_open(path); if (fd < 0) return NULL;
    FILE *file = _fdopen(fd, "rb");
    if (!file) close(fd);
    return file;
#else
    return fopen(path, "rb");
#endif
}

/* 1 directory, 0 missing, -1 other file or inspection failure. */
static inline int lmb_plan_optional_directory(const char *path) {
#ifdef _WIN32
    wchar_t *wide = lmb_plan_wide(path); if (!wide) return -1;
    struct _stat64 st; int rc = _wstat64(wide, &st); free(wide);
    if (rc) return errno == ENOENT ? 0 : -1;
    return (st.st_mode & _S_IFMT) == _S_IFDIR ? 1 : -1;
#else
    struct stat st;
    if (stat(path, &st)) return errno == ENOENT ? 0 : -1;
    return S_ISDIR(st.st_mode) ? 1 : -1;
#endif
}
#endif
