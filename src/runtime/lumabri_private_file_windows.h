/* Local Windows identity files: protected owner ACL, no reparse traversal,
 * no inherited handles, and bounded cross-process locking. */
#ifndef LUMABRI_PRIVATE_FILE_WINDOWS_H
#define LUMABRI_PRIVATE_FILE_WINDOWS_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    TOKEN_USER *user;
    PSECURITY_DESCRIPTOR descriptor;
    SECURITY_ATTRIBUTES attributes;
} LmbPrivateSecurity;

static int lmb_private_error(DWORD e) {
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT :
            e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? EEXIST :
            e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ? EBUSY :
            e == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EACCES;
    return -1;
}

static void lmb_private_security_free(LmbPrivateSecurity *s) {
    if (s->descriptor) LocalFree(s->descriptor);
    free(s->user); memset(s, 0, sizeof *s);
}

static int lmb_private_security(LmbPrivateSecurity *s) {
    memset(s, 0, sizeof *s);
    HANDLE token = NULL; DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return lmb_private_error(GetLastError());
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    s->user = size ? malloc(size) : NULL;
    if (!s->user || !GetTokenInformation(token, TokenUser, s->user, size, &size)) {
        CloseHandle(token); lmb_private_security_free(s); errno = EACCES; return -1;
    }
    CloseHandle(token);
    wchar_t *sid = NULL, text[512];
    if (!ConvertSidToStringSidW(s->user->User.Sid, &sid)) {
        lmb_private_security_free(s); errno = EACCES; return -1;
    }
    int n = swprintf(text, sizeof text / sizeof *text,
                     L"O:%lsD:P(A;;FA;;;%ls)(A;;FA;;;SY)", sid, sid);
    LocalFree(sid);
    if (n < 0 || !ConvertStringSecurityDescriptorToSecurityDescriptorW(
                       text, SDDL_REVISION_1, &s->descriptor, NULL)) {
        lmb_private_security_free(s); errno = EACCES; return -1;
    }
    s->attributes.nLength = sizeof s->attributes;
    s->attributes.lpSecurityDescriptor = s->descriptor;
    s->attributes.bInheritHandle = FALSE;
    return 0;
}

static int lmb_private_validate(HANDLE h, PSID user, int directory) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != !!directory ||
        (!directory && info.nNumberOfLinks != 1)) { errno = EACCES; return -1; }
    PSID owner = NULL; PACL acl = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    DWORD result = GetSecurityInfo(h, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, NULL, &acl, NULL, &sd);
    SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
    int good = !result && owner && EqualSid(owner, user) && acl &&
        GetSecurityDescriptorControl(sd, &control, &revision) && (control & SE_DACL_PROTECTED);
    for (DWORD i = 0; good && i < acl->AceCount; i++) {
        void *raw = NULL;
        if (!GetAce(acl, i, &raw)) { good = 0; break; }
        ACE_HEADER *head = raw;
        /* Reject unfamiliar/callback/object ACEs instead of guessing their rights. */
        if (head->AceType != ACCESS_ALLOWED_ACE_TYPE && head->AceType != ACCESS_DENIED_ACE_TYPE) {
            good = 0; break;
        }
        if (head->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            ACCESS_ALLOWED_ACE *ace = raw; PSID sid = &ace->SidStart;
            if (!IsValidSid(sid) || (!EqualSid(sid, user) && !IsWellKnownSid(sid, WinLocalSystemSid)))
                good = 0;
        }
    }
    if (sd) LocalFree(sd);
    if (!good) { errno = EACCES; return -1; }
    return 0;
}

typedef struct { wchar_t path[1024]; HANDLE parent[128]; size_t count; } LmbPrivatePath;

static void lmb_private_path_close(LmbPrivatePath *p) {
    while (p->count) CloseHandle(p->parent[--p->count]);
}

static int lmb_private_path(const char *path, LmbPrivatePath *p) {
    memset(p, 0, sizeof *p);
    wchar_t input[1024];
    if (!path || !*path || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            path, -1, input, sizeof input / sizeof *input)) { errno = EINVAL; return -1; }
    DWORD length = GetFullPathNameW(input, sizeof p->path / sizeof *p->path, p->path, NULL);
    if (!length || length >= sizeof p->path / sizeof *p->path ||
        !((p->path[0] >= L'A' && p->path[0] <= L'Z') ||
          (p->path[0] >= L'a' && p->path[0] <= L'z')) ||
        p->path[1] != L':' || p->path[2] != L'\\') { errno = EINVAL; return -1; }
    wchar_t root[4] = {p->path[0], L':', L'\\', 0};
    UINT drive = GetDriveTypeW(root);
    if (drive != DRIVE_FIXED && drive != DRIVE_RAMDISK) { errno = EACCES; return -1; }
    /* Credentials must live on a local drive, never a device, ADS or share. */
    for (DWORD i = 3; i < length; i++) {
        if (p->path[i] == L':' || p->path[i] == L'*' || p->path[i] == L'?') {
            errno = EINVAL; goto fail;
        }
        if (p->path[i] != L'\\') continue;
        if (p->count == sizeof p->parent / sizeof *p->parent) { errno = ENAMETOOLONG; goto fail; }
        p->path[i] = 0;
        HANDLE h = CreateFileW(p->path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        p->path[i] = L'\\';
        if (h == INVALID_HANDLE_VALUE) { lmb_private_error(GetLastError()); goto fail; }
        p->parent[p->count++] = h;
        FILE_ATTRIBUTE_TAG_INFO info;
        if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof info) ||
            !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { errno = EACCES; goto fail; }
    }
    return 0;
fail:
    lmb_private_path_close(p); return -1;
}

static int lmb_private_mkdir(const char *path) {
    LmbPrivatePath p; LmbPrivateSecurity s;
    if (lmb_private_path(path, &p)) return -1;
    if (lmb_private_security(&s)) { lmb_private_path_close(&p); return -1; }
    int rc = 0;
    if (!CreateDirectoryW(p.path, &s.attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        rc = lmb_private_error(GetLastError());
    if (!rc) {
        HANDLE h = CreateFileW(p.path, READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (h == INVALID_HANDLE_VALUE) rc = lmb_private_error(GetLastError());
        else { rc = lmb_private_validate(h, s.user->User.Sid, 1); CloseHandle(h); }
    }
    lmb_private_security_free(&s); lmb_private_path_close(&p); return rc;
}

/* Keep a handle to each parent until the final file is opened and checked;
 * lack of FILE_SHARE_DELETE prevents a concurrent parent rename/replacement. */
static HANDLE lmb_private_open_sharing(const char *path, int write, int create, int *created, DWORD sharing) {
    LmbPrivatePath p; LmbPrivateSecurity s;
    if (created) *created = 0;
    if (lmb_private_path(path, &p)) return INVALID_HANDLE_VALUE;
    if (lmb_private_security(&s)) { lmb_private_path_close(&p); return INVALID_HANDLE_VALUE; }
    HANDLE h = CreateFileW(p.path, READ_CONTROL | GENERIC_READ | (write ? GENERIC_WRITE : 0),
        sharing, &s.attributes, create ? OPEN_ALWAYS : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD error = GetLastError();
    if (h == INVALID_HANDLE_VALUE) lmb_private_error(error);
    else if (lmb_private_validate(h, s.user->User.Sid, 0)) {
        CloseHandle(h); h = INVALID_HANDLE_VALUE;
    } else if (created) *created = create && error != ERROR_ALREADY_EXISTS;
    lmb_private_security_free(&s); lmb_private_path_close(&p);
    return h;
}

static HANDLE lmb_private_open(const char *path,int write,int create,int *created) {
    return lmb_private_open_sharing(path,write,create,created,FILE_SHARE_READ|FILE_SHARE_WRITE);
}

static int lmb_private_lock(HANDLE h, int exclusive) {
    ULONGLONG deadline = GetTickCount64() + 5000;
    OVERLAPPED at = {0};
    for (;;) {
        if (LockFileEx(h, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0),
                       0, MAXDWORD, MAXDWORD, &at)) return 0;
        DWORD error = GetLastError();
        if (error != ERROR_LOCK_VIOLATION) return lmb_private_error(error);
        if (GetTickCount64() >= deadline) { errno = ETIMEDOUT; return -1; }
        Sleep(5);
    }
}

static void lmb_private_unlock(HANDLE h) {
    OVERLAPPED at = {0}; (void)UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &at);
}

static FILE *lmb_private_stream(const char *path, int write, int create) {
    HANDLE h = lmb_private_open(path, write, create, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (lmb_private_lock(h, write)) { CloseHandle(h); return NULL; }
    int fd = _open_osfhandle((intptr_t)h, (write ? _O_RDWR : _O_RDONLY) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) { lmb_private_unlock(h); CloseHandle(h); return NULL; }
    FILE *fp = _fdopen(fd, write ? "r+b" : "rb");
    if (!fp) { lmb_private_unlock(h); _close(fd); }
    return fp;
}

static int lmb_private_stream_flush(FILE *fp) {
    return fflush(fp) || !FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(fp))) ? -1 : 0;
}
static void lmb_private_stream_close(FILE *fp) {
    /* Flush while the exclusive byte lock is still held. */
    (void)fflush(fp);
    lmb_private_unlock((HANDLE)_get_osfhandle(_fileno(fp))); fclose(fp);
}

static int lmb_private_default_path(char *out, size_t cap, const char *name) {
    wchar_t local[512]; char utf8[1536], dir[1600];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, sizeof local / sizeof *local);
    if (!n || n >= sizeof local / sizeof *local ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, local, -1, utf8, sizeof utf8, NULL, NULL)) {
        errno = EINVAL; return -1;
    }
    if (snprintf(dir, sizeof dir, "%s\\Lumabri", utf8) >= (int)sizeof dir ||
        lmb_private_mkdir(dir)) return -1;
    int written = snprintf(out, cap, "%s\\%s", dir, name);
    if (written < 0 || (size_t)written >= cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
#endif
#endif
