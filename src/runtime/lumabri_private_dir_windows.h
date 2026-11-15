/* Anchored private directories for the stores that POSIX keeps behind a
 * directory descriptor (API access records, capabilities, chat history).
 * The same store code runs on both systems: this file supplies the handful
 * of descriptor-relative calls it uses, on top of lumabri_private_file_windows.h.
 *
 * What "owner only" means here is the Windows equivalent of mode 0600/0700,
 * checked on the open handle: owned by this user, a protected DACL (nothing
 * inherited) that grants access to this user and LocalSystem only, no reparse
 * point anywhere on the path, a local fixed drive, and a single hard link for
 * files. fstat() reports such an object as 0600/0700 and anything else as
 * world-accessible, so every `st_mode & 077` and ownership check in the shared
 * stores refuses it exactly as it refuses a group-readable file on POSIX.
 *
 * A directory descriptor is a C runtime descriptor over a directory handle
 * opened without FILE_SHARE_DELETE: the directory cannot be renamed or
 * replaced while it is held, and names are resolved below its final path. */
#ifndef LUMABRI_PRIVATE_DIR_WINDOWS_H
#define LUMABRI_PRIVATE_DIR_WINDOWS_H
#ifdef _WIN32
#include "lumabri_private_file_windows.h"
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0 /* reparse points are always refused, never followed */
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0 /* these descriptors are never inheritable */
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef LOCK_EX
#define LOCK_EX 2
#define LOCK_NB 4
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

static inline short lmb_private_euid(void) { return 0; } /* the type of st_uid here */
#define geteuid lmb_private_euid

static int lmb_private_dir_open(const char *path, int create) {
    if (create && lmb_private_mkdir(path)) return -1;
    LmbPrivatePath p; LmbPrivateSecurity s;
    if (lmb_private_path(path, &p)) return -1;
    if (lmb_private_security(&s)) { lmb_private_path_close(&p); return -1; }
    HANDLE h = CreateFileW(p.path, READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    int fd = -1;
    if (h == INVALID_HANDLE_VALUE) lmb_private_error(GetLastError());
    else if (lmb_private_validate(h, s.user->User.Sid, 1)) CloseHandle(h);
    else if ((fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY | _O_NOINHERIT)) < 0) {
        CloseHandle(h); errno = EMFILE;
    }
    lmb_private_security_free(&s); lmb_private_path_close(&p);
    return fd;
}

/* One plain name below the held directory; "." is the directory itself and
 * ".." its parent. Separators, drive and stream syntax are never accepted. */
static int lmb_private_dir_path(int dir, const char *name, char *out, size_t cap) {
    HANDLE h = (HANDLE)_get_osfhandle(dir);
    wchar_t wide[1100]; char base[3300];
    if (h == INVALID_HANDLE_VALUE || !name || !*name) { errno = EBADF; return -1; }
    DWORD n = GetFinalPathNameByHandleW(h, wide, sizeof wide / sizeof *wide,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= sizeof wide / sizeof *wide || wcsncmp(wide, L"\\\\?\\", 4) ||
        !wide[4] || wide[5] != L':' ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide + 4, -1, base, sizeof base, NULL, NULL)) {
        errno = EACCES; return -1;
    }
    int written;
    if (!strcmp(name, ".")) written = snprintf(out, cap, "%s", base);
    else if (!strcmp(name, "..")) {
        char *slash = strrchr(base, '\\');
        if (!slash || slash - base < 3) { errno = EACCES; return -1; }
        *slash = 0; written = snprintf(out, cap, "%s", base);
    } else {
        if (strpbrk(name, "\\/:*?\"<>|")) { errno = EINVAL; return -1; }
        written = snprintf(out, cap, "%s\\%s", base, name);
    }
    if (written < 0 || (size_t)written >= cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int openat(int dir, const char *name, int flags, ...) {
    char path[3400];
    if (lmb_private_dir_path(dir, name, path, sizeof path)) return -1;
    if (flags & O_DIRECTORY) return lmb_private_dir_open(path, 0);
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0, created = 0;
    /* Readers may hold a record while its replacement is renamed over it. */
    HANDLE h = lmb_private_open_sharing(path, writing, (flags & O_CREAT) != 0, &created,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if ((flags & O_EXCL) && !created) { CloseHandle(h); errno = EEXIST; return -1; }
    int fd = _open_osfhandle((intptr_t)h, (writing ? _O_RDWR : _O_RDONLY) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) { CloseHandle(h); errno = EMFILE; }
    return fd;
}

static int mkdirat(int dir, const char *name, int mode) {
    (void)mode; char path[3400];
    return lmb_private_dir_path(dir, name, path, sizeof path) ? -1 : lmb_private_mkdir(path);
}

static int lmb_private_dir_wide(int dir, const char *name, wchar_t *out, size_t cap) {
    char path[3400];
    if (lmb_private_dir_path(dir, name, path, sizeof path)) return -1;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, (int)cap)) { errno = EINVAL; return -1; }
    return 0;
}

/* A reader's handle on the old record is open for microseconds; wait for it
 * instead of failing an atomic replacement. */
static int renameat(int from_dir, const char *from, int to_dir, const char *to) {
    wchar_t source[1100], target[1100];
    if (lmb_private_dir_wide(from_dir, from, source, 1100) || lmb_private_dir_wide(to_dir, to, target, 1100)) return -1;
    for (unsigned attempt = 0;; attempt++) {
        if (MoveFileExW(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
        DWORD error = GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) || attempt >= 200)
            return lmb_private_error(error);
        Sleep(5);
    }
}

static int unlinkat(int dir, const char *name, int flags) {
    (void)flags; wchar_t path[1100];
    if (lmb_private_dir_wide(dir, name, path, 1100)) return -1;
    for (unsigned attempt = 0;; attempt++) {
        if (DeleteFileW(path)) return 0;
        DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION || attempt >= 200) return lmb_private_error(error);
        Sleep(5);
    }
}

/* `struct stat` is the caller's type: the C runtimes disagree on whether it
 * is `struct _stat64`, so the runtime's own answer is copied field by field. */
static int lmb_private_fstat(int fd, struct stat *st) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    BY_HANDLE_FILE_INFORMATION info;
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    /* Pipes and consoles keep the C runtime's answer. */
    if (GetFileType(h) != FILE_TYPE_DISK) {
        struct _stat64 raw;
        if (_fstat64(fd, &raw)) return -1;
        memset(st, 0, sizeof *st);
        st->st_mode = raw.st_mode; st->st_nlink = raw.st_nlink; st->st_size = raw.st_size;
        st->st_mtime = raw.st_mtime; st->st_atime = raw.st_atime; st->st_ctime = raw.st_ctime;
        return 0;
    }
    if (!GetFileInformationByHandle(h, &info)) { errno = EACCES; return -1; }
    int directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    LmbPrivateSecurity s;
    if (lmb_private_security(&s)) return -1;
    int private = !lmb_private_validate(h, s.user->User.Sid, directory);
    lmb_private_security_free(&s);
    memset(st, 0, sizeof *st);
    st->st_mode = (unsigned short)((directory ? S_IFDIR : S_IFREG) | (private ? (directory ? 0700 : 0600) : 0666));
    st->st_nlink = (short)(info.nNumberOfLinks > 32767 ? 32767 : info.nNumberOfLinks);
    st->st_size = (long long)(((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow);
    unsigned long long ticks = ((unsigned long long)info.ftLastWriteTime.dwHighDateTime << 32) |
        info.ftLastWriteTime.dwLowDateTime;
    st->st_mtime = ticks > 116444736000000000ULL ? (long long)((ticks - 116444736000000000ULL) / 10000000ULL) : 0;
    st->st_atime = st->st_ctime = st->st_mtime;
    return 0;
}
#undef fstat
#define fstat lmb_private_fstat

static int fstatat(int dir, const char *name, struct stat *st, int flags) {
    (void)flags;
    int fd = openat(dir, name, O_RDONLY);
    if (fd < 0) return -1;
    int rc = lmb_private_fstat(fd, st), saved = errno;
    close(fd); errno = saved; return rc;
}

static int fsync(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    BY_HANDLE_FILE_INFORMATION info;
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &info)) { errno = EBADF; return -1; }
    /* Renames and deletions above are write-through; a directory handle has
     * nothing further to flush. */
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    if (!FlushFileBuffers(h)) { errno = EIO; return -1; }
    return 0;
}

/* Whole-file advisory lock, released when the descriptor closes or the
 * process exits. Separate handles in one process exclude each other, which
 * is what the threaded gateway needs. */
static int flock(int fd, int operation) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED at = {0};
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | ((operation & LOCK_NB) ? LOCKFILE_FAIL_IMMEDIATELY : 0);
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &at)) return 0;
    DWORD error = GetLastError();
    errno = error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING ? EWOULDBLOCK : EACCES;
    return -1;
}

/* Names only; the caller opens and checks each entry it cares about. */
static int lmb_private_dir_each(int dir, int (*visit)(const char *name, void *context), void *context) {
    wchar_t pattern[1100]; WIN32_FIND_DATAW entry;
    if (lmb_private_dir_wide(dir, ".", pattern, 1098)) return -1;
    wcscat(pattern, L"\\*");
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    int rc = 0;
    do {
        char name[800];
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        rc = visit(name, context);
    } while (!rc && FindNextFileW(find, &entry));
    if (!rc && GetLastError() != ERROR_NO_MORE_FILES) rc = -1;
    FindClose(find); return rc;
}
#endif
#endif
