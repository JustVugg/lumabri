/* Native same-user service control. The wire records and mutation fences are
 * shared with Unix; no PID from a journal grants authority over a process. */
#ifndef LUMABRI_SERVICE_WINDOWS_H
#define LUMABRI_SERVICE_WINDOWS_H
#include "src/runtime/lumabri_pipe_windows.h"
#include "src/crypto/lumabri_sign_core.h"
#include "src/crypto/lumabri_random.h"

typedef struct {
    int fd;
    uint8_t instance[32];
    int (*save)(int fd, const HomeServiceSnapshot *snapshot);
} HomePreparationObserver;

typedef struct {
    HANDLE lock, listener, reply;
    OVERLAPPED connection;
    int connecting, connected;
    char directory[1200], socket_path[1200], journal[1200];
    HomeServiceSnapshot snapshot;
    uint8_t *last;
    size_t last_size;
    uint32_t batch_completed, batch_total;
    HomePreparationObserver observer;
} HomeService;

static int home_service_directory(char *out, size_t cap) {
    return lmb_private_default_path(out, cap, "service") || lmb_private_mkdir(out) ? -1 : 0;
}

static int home_service_path(const char *role, const char *suffix, char *out, size_t cap) {
    char directory[1200];
    if (!home_service_role(role) || home_service_directory(directory, sizeof directory)) return -1;
    int n = snprintf(out, cap, "%s\\%s.%s", directory, role, suffix);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static int home_service_socket_path(const char *role, char *out, size_t cap) {
    char directory[1200];
    if (!home_service_role(role) || home_service_directory(directory, sizeof directory)) return -1;
    return lmb_pipe_name(directory,role,out,cap);
}

#define home_service_peer lmb_pipe_peer
#define home_service_server lmb_pipe_server
#define home_service_io lmb_pipe_io

static void home_service_disconnect(HomeService *s) {
    if (s->connecting) {
        DWORD ignored;
        CancelIoEx(s->listener, &s->connection);
        (void)GetOverlappedResult(s->listener, &s->connection, &ignored, TRUE);
    }
    if (s->listener != INVALID_HANDLE_VALUE) DisconnectNamedPipe(s->listener);
    s->connecting = s->connected = 0; s->reply = INVALID_HANDLE_VALUE;
}

static void home_service_close(HomeService *s) {
    home_service_disconnect(s);
    if (s->listener != INVALID_HANDLE_VALUE) CloseHandle(s->listener);
    if (s->connection.hEvent) CloseHandle(s->connection.hEvent);
    if (s->lock != INVALID_HANDLE_VALUE) CloseHandle(s->lock);
    free(s->last); s->last = NULL; s->last_size = 0;
    s->reply = s->listener = s->lock = INVALID_HANDLE_VALUE;
    s->connection.hEvent = NULL;
}

static int home_service_open(HomeService *s, const char *role) {
    memset(s, 0, sizeof *s); s->lock = s->listener = s->reply = INVALID_HANDLE_VALUE;
    char lock[1200]; wchar_t pipe[256]; LmbPrivateSecurity security;
    if (home_service_directory(s->directory, sizeof s->directory) ||
        home_service_path(role, "lock", lock, sizeof lock) ||
        home_service_path(role, "state", s->journal, sizeof s->journal) ||
        home_service_socket_path(role, s->socket_path, sizeof s->socket_path)) goto bad;
    s->lock = lmb_private_open(lock, 1, 1, NULL);
    if (s->lock == INVALID_HANDLE_VALUE) goto bad;
    OVERLAPPED at = {0};
    if (!LockFileEx(s->lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, MAXDWORD, MAXDWORD, &at)) { errno = EBUSY; goto bad; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s->socket_path, -1, pipe, 256) ||
        lmb_private_security(&security)) goto bad;
    s->listener = CreateNamedPipeW(pipe,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, HOME_SVC_MAX + 4, 80, 200, &security.attributes);
    lmb_private_security_free(&security);
    if (s->listener == INVALID_HANDLE_VALUE) goto bad;
    s->connection.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s->connection.hEvent) goto bad;
    snprintf(s->snapshot.role, sizeof s->snapshot.role, "%s", role);
    s->snapshot.pid = GetCurrentProcessId(); s->snapshot.state = HOME_SVC_RUNNING;
    lmb_random(s->snapshot.instance, 32); return 0;
bad:
    home_service_close(s); return -1;
}

static HANDLE home_service_connect(const char *role) {
    char path[1200]; wchar_t pipe[256];
    if (home_service_socket_path(role, path, sizeof path) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, pipe, 256)) return INVALID_HANDLE_VALUE;
    ULONGLONG deadline = GetTickCount64() + 200;
    for (;;) {
        /* Identification lets the server check our user, not act as us. */
        HANDLE h = CreateFileW(pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            if (!home_service_server(h)) return h;
            CloseHandle(h); errno = EACCES; return INVALID_HANDLE_VALUE;
        }
        DWORD error = GetLastError(); ULONGLONG now = GetTickCount64();
        if (error != ERROR_PIPE_BUSY || now >= deadline ||
            !WaitNamedPipeW(pipe, (DWORD)(deadline - now))) return INVALID_HANDLE_VALUE;
    }
}

static int home_service_query(const char *role, uint32_t op,
        const HomeServiceSnapshot *expected, HomeServiceSnapshot *out) {
    uint8_t request[80], incarnation[32];
    if (!out || home_service_request_pack(request, op, expected)) return -1;
    if (expected) memcpy(incarnation, expected->instance, 32);
    HANDLE pipe = home_service_connect(role);
    if (pipe == INVALID_HANDLE_VALUE) return -1;
    uint8_t header[4], bytes[HOME_SVC_MAX]; ULONGLONG deadline = GetTickCount64() + 2000;
    int rc = home_service_io(pipe, request, sizeof request, 1, deadline) ||
             home_service_io(pipe, header, sizeof header, 0, deadline);
    uint32_t length = rc ? 0 : lmb_get32(header);
    if (!rc) rc = !length || length > sizeof bytes ||
        home_service_io(pipe, bytes, length, 0, deadline) || home_service_unpack(bytes, length, out) ||
        strcmp(out->role, role) || (expected && memcmp(out->instance, incarnation, 32));
    /* Windows disconnect discards unread pipe data. A bounded receipt avoids
     * FlushFileBuffers (which could let a stalled client hang the keeper). */
    uint8_t receipt = 1;
    if (!rc) rc = home_service_io(pipe, &receipt, 1, 1, deadline);
    CloseHandle(pipe); return rc ? -1 : 0;
}

static int home_service_poll(HomeService *s) {
    if (s->reply != INVALID_HANDLE_VALUE) return -1;
    if (!s->connected && !s->connecting) {
        ResetEvent(s->connection.hEvent);
        if (ConnectNamedPipe(s->listener, &s->connection)) s->connected = 1;
        else {
            DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) s->connected = 1;
            else if (error == ERROR_IO_PENDING) s->connecting = 1;
            else { home_service_disconnect(s); return -1; }
        }
    }
    if (s->connecting) {
        DWORD ignored;
        if (WaitForSingleObject(s->connection.hEvent, 0) != WAIT_OBJECT_0) return -1;
        s->connecting = 0;
        if (!GetOverlappedResult(s->listener, &s->connection, &ignored, FALSE)) {
            home_service_disconnect(s); return -1;
        }
        s->connected = 1;
    }
    uint8_t request[80];
    if (home_service_io(s->listener, request, sizeof request, 0, GetTickCount64() + 200) ||
        home_service_peer(s->listener)) { home_service_disconnect(s); return -1; }
    int op = home_service_request_operation(request, &s->snapshot);
    if (op < 0) { home_service_disconnect(s); return -1; }
    s->reply = s->listener; return op;
}

static void home_service_answer(HomeService *s) {
    if (s->reply == INVALID_HANDLE_VALUE) return;
    LmbBuf b = {0}; uint8_t header[4], receipt; ULONGLONG deadline = GetTickCount64() + 200;
    if (!home_service_pack(&b, &s->snapshot)) {
        lmb_put32(header, (uint32_t)b.len);
        if (!home_service_io(s->reply, header, sizeof header, 1, deadline) &&
            !home_service_io(s->reply, b.p, b.len, 1, deadline))
            (void)home_service_io(s->reply, &receipt, 1, 0, deadline);
    }
    free(b.p); home_service_disconnect(s);
}

static int home_service_save(HomeService *s) {
    LmbBuf b = {0}; HomeServiceSnapshot journal = s->snapshot;
    journal.compute_active = journal.compute_queued = 0; journal.compute_grants = 0;
    if (home_service_pack(&b, &journal)) { free(b.p); return -1; }
    if (b.len == s->last_size && s->last && !memcmp(b.p, s->last, b.len)) {
        free(b.p); return s->observer.save ? s->observer.save(s->observer.fd, &s->snapshot) : 0;
    }
    LmbPrivatePath target; LmbPrivateSecurity security;
    if (lmb_private_path(s->journal, &target)) { free(b.p); return -1; }
    if (lmb_private_security(&security)) { lmb_private_path_close(&target); free(b.p); return -1; }
    uint8_t random[16]; char hex[33]; wchar_t temporary[1100];
    lmb_random(random, sizeof random); lmb_hex(hex, random, sizeof random);
    int n = swprintf(temporary, sizeof temporary / sizeof *temporary, L"%ls.%hs.tmp", target.path, hex);
    HANDLE h = n < 0 ? INVALID_HANDLE_VALUE : CreateFileW(temporary, GENERIC_WRITE, 0,
        &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    int rc = -1;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD count;
        rc = !WriteFile(h, b.p, (DWORD)b.len, &count, NULL) || count != b.len || !FlushFileBuffers(h);
        if (!CloseHandle(h)) rc = -1;
        if (!rc) {
            /* ReplaceFile preserves the existing ACL. Reject unsafe state
             * first; never propagate a public or foreign-owned journal. */
            HANDLE existing = lmb_private_open(s->journal, 0, 0, NULL);
            if (existing != INVALID_HANDLE_VALUE) {
                CloseHandle(existing);
                if (!ReplaceFileW(target.path, temporary, NULL, 0, NULL, NULL)) rc = -1;
            } else if (errno != ENOENT ||
                !MoveFileExW(temporary, target.path, MOVEFILE_WRITE_THROUGH)) rc = -1;
        }
        if (rc) DeleteFileW(temporary);
    }
    lmb_private_security_free(&security); lmb_private_path_close(&target);
    if (!rc) { free(s->last); s->last = b.p; s->last_size = b.len; }
    else free(b.p);
    if (!rc && s->observer.save) rc = s->observer.save(s->observer.fd, &s->snapshot);
    return rc ? -1 : 0;
}

static int home_service_record(const char *role, HomeServiceSnapshot *out) {
    char path[1200];
    if (home_service_path(role, "state", path, sizeof path)) return -1;
    LmbPrivatePath resolved; LmbPrivateSecurity security;
    if (lmb_private_path(path, &resolved)) return -1;
    if (lmb_private_security(&security)) { lmb_private_path_close(&resolved); return -1; }
    /* A reader holds one immutable file version. Permit atomic replacement
     * by the keeper, unlike credential files and the singleton lock. */
    HANDLE h = CreateFileW(resolved.path, GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE && lmb_private_validate(h, security.user->User.Sid, 0)) {
        CloseHandle(h); h = INVALID_HANDLE_VALUE;
    }
    lmb_private_security_free(&security); lmb_private_path_close(&resolved);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size; uint8_t bytes[HOME_SVC_MAX]; DWORD count;
    int rc = !GetFileSizeEx(h, &size) || size.QuadPart <= 0 || (uint64_t)size.QuadPart > sizeof bytes;
    if (!rc) rc = !ReadFile(h, bytes, (DWORD)size.QuadPart, &count, NULL) || count != size.QuadPart ||
        home_service_unpack(bytes, count, out) || strcmp(out->role, role);
    CloseHandle(h); return rc ? -1 : 0;
}
#endif
