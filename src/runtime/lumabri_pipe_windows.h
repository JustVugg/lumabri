/* Private local named-pipe primitives shared by service control and compute
 * admission. Same-user peer checks and bounded overlapped I/O; no TCP port. */
#ifndef LUMABRI_PIPE_WINDOWS_H
#define LUMABRI_PIPE_WINDOWS_H
#include "src/runtime/lumabri_private_file_windows.h"
#include "src/crypto/lumabri_sign_core.h"

static inline int lmb_pipe_name(const char *location, const char *role, char *out, size_t cap) {
    LmbPrivatePath path; LmbPrivateSecurity security; char hex[65];
    if (!role || !*role || strlen(role)>32) return -1;
    for (const char *s=role; *s; s++) if ((*s<'a' || *s>'z') && *s!='-') return -1;
    if (lmb_private_path(location, &path)) return -1;
    if (lmb_private_security(&security)) { lmb_private_path_close(&path); return -1; }
    CharLowerBuffW(path.path, (DWORD)wcslen(path.path));
    uint8_t digest[64]; LmbSha512 sha;
    lmb_sha512_init(&sha);
    lmb_sha512_update(&sha, path.path, wcslen(path.path) * sizeof(wchar_t));
    lmb_sha512_update(&sha, security.user->User.Sid, GetLengthSid(security.user->User.Sid));
    lmb_sha512_final(&sha, digest); lmb_hex(hex, digest, 32);
    lmb_private_security_free(&security); lmb_private_path_close(&path);
    int n = snprintf(out, cap, "\\\\.\\pipe\\lumabri-%s-%s", hex, role);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static inline int lmb_pipe_token_user(HANDLE token, PSID wanted) {
    DWORD size = 0; GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = size ? malloc(size) : NULL;
    int same = user && GetTokenInformation(token, TokenUser, user, size, &size) &&
               EqualSid(user->User.Sid, wanted);
    free(user); return same ? 0 : -1;
}

/* Server: identify the token associated with the request actually read from
 * the pipe. Never execute an operation while impersonating the client. */
static inline int lmb_pipe_peer(HANDLE pipe) {
    LmbPrivateSecurity security;
    if (lmb_private_security(&security)) return -1;
    HANDLE token = NULL; int rc = -1;
    if (ImpersonateNamedPipeClient(pipe)) {
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
            rc = lmb_pipe_token_user(token, security.user->User.Sid);
        if (!RevertToSelf()) abort(); /* Cannot safely continue under another token. */
    }
    if (token) CloseHandle(token);
    lmb_private_security_free(&security); return rc;
}

/* Client: the pipe's server process is resolved by the kernel. A PID read
 * from disk is never used, and the handle is only opened for observation. */
static inline int lmb_pipe_server(HANDLE pipe) {
    ULONG pid = 0, again = 0; HANDLE process = NULL, token = NULL;
    LmbPrivateSecurity security; int rc = -1;
    if (lmb_private_security(&security)) return -1;
    if (GetNamedPipeServerProcessId(pipe, &pid))
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process && OpenProcessToken(process, TOKEN_QUERY, &token) &&
        !lmb_pipe_token_user(token, security.user->User.Sid) &&
        GetNamedPipeServerProcessId(pipe, &again) && again == pid &&
        WaitForSingleObject(process, 0) == WAIT_TIMEOUT) rc = 0;
    if (token) CloseHandle(token);
    if (process) CloseHandle(process);
    lmb_private_security_free(&security); return rc;
}

/* Absolute deadlines cover partial frames and slow readers/writers. Every
 * pending operation is cancelled and reaped before its OVERLAPPED expires. */
static inline int lmb_pipe_io(HANDLE pipe, void *bytes, size_t length, int writing,
                           ULONGLONG deadline) {
    uint8_t *p = bytes; OVERLAPPED operation = {0}; int rc = -1;
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!operation.hEvent) return -1;
    while (length) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { errno = ETIMEDOUT; goto done; }
        ResetEvent(operation.hEvent); DWORD count = 0;
        DWORD size = length > MAXDWORD ? MAXDWORD : (DWORD)length;
        BOOL ok = writing ? WriteFile(pipe, p, size, &count, &operation) :
                            ReadFile(pipe, p, size, &count, &operation);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) goto done;
            DWORD wait = WaitForSingleObject(operation.hEvent,
                deadline - now > MAXDWORD - 1 ? MAXDWORD - 1 : (DWORD)(deadline - now));
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                (void)GetOverlappedResult(pipe, &operation, &count, TRUE);
                errno = wait == WAIT_TIMEOUT ? ETIMEDOUT : EIO; goto done;
            }
            if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) goto done;
        }
        if (!count) goto done;
        p += count; length -= count;
    }
    rc = 0;
done:
    CloseHandle(operation.hEvent); return rc;
}

#endif
