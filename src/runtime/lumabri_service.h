/* Private, versioned local control plane. A keeper owns its children and
 * leases; neither a terminal nor the replaceable manager owns the keeper.
 * Journal entries are observations, NEVER permission to replay an OFFER.
 * No persisted PID is ever used as authority to signal a process. */
#ifndef LUMABRI_SERVICE_H
#define LUMABRI_SERVICE_H
#include "src/protocol/lumabri_service_wire.h"
#ifdef _WIN32
#include "src/runtime/lumabri_service_windows.h"
#else
#include <sys/un.h>
#include <sys/file.h>

/* Optional durable observer for a preparation submitted through another
 * interface. The keeper inherits only this explicit descriptor, never the
 * HTTP connection. Observations are not authority to replay an operation. */
typedef struct {
    int fd;
    uint8_t instance[32];
    int (*save)(int fd, const HomeServiceSnapshot *snapshot);
} HomePreparationObserver;

typedef struct {
    int lock, listener, reply;
    char directory[1200], socket_path[1200], journal[1200];
    HomeServiceSnapshot snapshot;
    uint8_t *last;
    size_t last_size;
    uint32_t batch_completed, batch_total; /* preparation only; detail is journalled */
    HomePreparationObserver observer;
} HomeService;

static int home_service_directory(char *dir, size_t cap) {
    const char *base = getenv("HOME");
    char parent[1200]; struct stat st;
    if (!base || !*base || checked_printf(parent, sizeof parent, "%s/.lumabri", base)) return -1;
    if (mkdir(parent, 0700) && errno != EEXIST) return -1;
    if (lstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 022)) return -1;
    if (checked_printf(dir, cap, "%s/service", parent)) return -1;
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    return lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) ? -1 : 0;
}

static int home_service_path(const char *role, const char *suffix, char *path, size_t cap) {
    char dir[1200];
    return !home_service_role(role) || home_service_directory(dir, sizeof dir) ||
        checked_printf(path, cap, "%s/%s.%s", dir, role, suffix) ? -1 : 0;
}

/* Darwin's sun_path is only 104 bytes; normal macOS temporary homes can
 * exceed that. Keep locks/journals in HOME and use a private short directory
 * for the socket only. Never chdir(): other threads may be doing file I/O.
 * Same-user kernel credentials still authenticate both ends. */
static int home_service_socket_path(const char *role, char *path, size_t cap) {
    char full[1200];
    if (home_service_path(role, "sock", full, sizeof full)) return -1;
    struct sockaddr_un limit;
    if (strlen(full) < sizeof limit.sun_path)
        return checked_printf(path, cap, "%s", full);
    char directory[1200], canonical[PATH_MAX], short_dir[96], hex[33];
    uint8_t digest[32]; LmbSha sha; struct stat st;
    if (home_service_directory(directory, sizeof directory) || !realpath(directory, canonical)) return -1;
    lmb_sha_init(&sha); lmb_sha_update(&sha, canonical, strlen(canonical)); lmb_sha_final(&sha, digest);
    lmb_hex(hex, digest, 16);
    if (checked_printf(short_dir, sizeof short_dir, "/tmp/lmb-ipc-%lu-%s", (unsigned long)geteuid(), hex)) return -1;
    if (mkdir(short_dir, 0700) && errno != EEXIST) return -1;
    if (lstat(short_dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) return -1;
    return checked_printf(path, cap, "%s/%s.sock", short_dir, role);
}

/* A private directory plus kernel peer credentials, not a claimed PID or an
 * HTTP header. The socket is never exposed on the LAN. */
static int home_service_peer(int fd) {
#ifdef __APPLE__
    uid_t uid; gid_t gid;
    return getpeereid(fd, &uid, &gid) || uid != geteuid() ? -1 : 0;
#elif defined(__linux__)
    struct ucred cred; socklen_t n = sizeof cred;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &n) ||
        n != sizeof cred || cred.uid != geteuid() ? -1 : 0;
#else
    (void)fd; errno = ENOTSUP; return -1;
#endif
}

/* Absolute deadline, including a peer sending one byte at a time. */
static int home_service_io(int fd, void *bytes, size_t length, int writing, double deadline) {
    uint8_t *p = bytes;
    while (length) {
        double remaining = deadline - nowd();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd f = {fd, writing ? POLLOUT : POLLIN, 0};
        int rc = poll(&f, 1, (int)(remaining * 1000) + 1);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        ssize_t n = writing ? send(fd, p, length, 0) : recv(fd, p, length, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int home_service_save(HomeService *s) {
    LmbBuf b = {0};
    /* Live queue counters are RPC observations, not restart state. Persisting
     * every kernel grant would add fsync traffic to resident inference. */
    HomeServiceSnapshot journal = s->snapshot;
    journal.compute_active = journal.compute_queued = 0; journal.compute_grants = 0;
    if (home_service_pack(&b, &journal)) { free(b.p); return -1; }
    if (b.len == s->last_size && s->last && !memcmp(b.p, s->last, b.len)) {
        free(b.p);
        return s->observer.save ? s->observer.save(s->observer.fd, &s->snapshot) : 0;
    }
    char tmp[1240];
    if (checked_printf(tmp, sizeof tmp, "%s.XXXXXX", s->journal)) { free(b.p); return -1; }
    int fd = mkstemp(tmp), rc = -1;
    if (fd >= 0) {
        rc = fchmod(fd, 0600);
        size_t off = 0;
        while (!rc && off < b.len) {
            ssize_t n = write(fd, b.p + off, b.len - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) rc = -1;
            else off += (size_t)n;
        }
        if (!rc && fsync(fd)) rc = -1;
        if (close(fd)) rc = -1;
        if (!rc && rename(tmp, s->journal)) rc = -1;
        if (!rc) {
            int directory = open(s->directory, O_RDONLY | O_CLOEXEC);
            if (directory < 0 || fsync(directory)) rc = -1;
            if (directory >= 0) close(directory);
        }
        if (rc) unlink(tmp);
    }
    if (!rc) { free(s->last); s->last = b.p; s->last_size = b.len; }
    else free(b.p);
    if (!rc && s->observer.save) rc = s->observer.save(s->observer.fd, &s->snapshot);
    return rc;
}

static int home_service_record(const char *role, HomeServiceSnapshot *s) {
    char path[1200]; struct stat st;
    if (home_service_path(role, "state", path, sizeof path)) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > HOME_SVC_MAX) { close(fd); return -1; }
    uint8_t bytes[HOME_SVC_MAX];
    int rc = lmb_read_full(fd, bytes, (size_t)st.st_size); close(fd);
    return rc || home_service_unpack(bytes, (size_t)st.st_size, s) || strcmp(s->role, role) ? -1 : 0;
}

static int home_service_connect(const char *role) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (home_service_socket_path(role, addr.sun_path, sizeof addr.sun_path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) { close(fd); return -1; }
    int connected = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    if (connected && errno == EINPROGRESS) {
        struct pollfd p = {fd, POLLOUT, 0}; int error = 0; socklen_t n = sizeof error;
        connected = poll(&p, 1, 200) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n) || error;
    }
    if (connected || home_service_peer(fd)) {
        close(fd); return -1;
    }
    return fd;
}

static int home_service_query(const char *role, uint32_t op,
    const HomeServiceSnapshot *expected, HomeServiceSnapshot *out) {
    uint8_t incarnation[32];
    if (expected) memcpy(incarnation, expected->instance, 32);
    int fd = home_service_connect(role);
    if (fd < 0) return -1;
    uint8_t request[80];
    if (home_service_request_pack(request, op, expected)) { close(fd); return -1; }
    uint8_t header[4], bytes[HOME_SVC_MAX]; double deadline = nowd() + 2;
    int rc = home_service_io(fd, request, sizeof request, 1, deadline) ||
        home_service_io(fd, header, sizeof header, 0, deadline);
    uint32_t length = rc ? 0 : lmb_get32(header);
    if (!rc) rc = !length || length > sizeof bytes ||
        home_service_io(fd, bytes, length, 0, deadline) || home_service_unpack(bytes, length, out) ||
        strcmp(out->role, role) || (expected && memcmp(out->instance, incarnation, 32));
    close(fd); return rc ? -1 : 0;
}

static void home_service_close(HomeService *s) {
    if (s->reply >= 0) close(s->reply);
    if (s->listener >= 0) { close(s->listener); unlink(s->socket_path); }
    if (s->lock >= 0) close(s->lock);
    free(s->last); s->last = NULL;
    s->reply = s->listener = s->lock = -1;
}

static int home_service_open(HomeService *s, const char *role) {
    memset(s, 0, sizeof *s); s->lock = s->listener = s->reply = -1;
    char lock_path[1200]; struct stat st;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (home_service_directory(s->directory, sizeof s->directory) ||
        home_service_path(role, "lock", lock_path, sizeof lock_path) ||
        home_service_socket_path(role, s->socket_path, sizeof s->socket_path) ||
        checked_printf(addr.sun_path, sizeof addr.sun_path, "%s", s->socket_path) ||
        home_service_path(role, "state", s->journal, sizeof s->journal)) goto bad;
    s->lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->lock < 0 || fstat(s->lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_nlink != 1 || flock(s->lock, LOCK_EX | LOCK_NB)) goto bad;
    if (!lstat(s->socket_path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || unlink(s->socket_path)) goto bad;
    } else if (errno != ENOENT) goto bad;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) goto bad;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK) ||
        bind(fd, (struct sockaddr *)&addr, sizeof addr)) { close(fd); goto bad; }
    s->listener = fd;
    if (chmod(s->socket_path, 0600) || listen(fd, 8)) goto bad;
    snprintf(s->snapshot.role, sizeof s->snapshot.role, "%s", role);
    s->snapshot.pid = (uint64_t)getpid(); s->snapshot.state = HOME_SVC_RUNNING;
    lmb_random(s->snapshot.instance, 32);
    return 0;
bad:
    home_service_close(s); return -1;
}

/* The caller applies the command before answering. Mutations are fenced by
 * keeper incarnation AND immutable allocation ID, including unload/stop. */
static int home_service_poll(HomeService *s) {
    int fd = accept(s->listener, NULL, NULL);
    if (fd < 0) return -1;
    uint8_t r[80];
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK) || home_service_peer(fd) ||
        home_service_io(fd, r, sizeof r, 0, nowd() + .2)) { close(fd); return -1; }
    int op = home_service_request_operation(r, &s->snapshot);
    if (op < 0) { close(fd); return -1; }
    s->reply = fd; return op;
}

static void home_service_answer(HomeService *s) {
    if (s->reply < 0) return;
    LmbBuf b = {0}; uint8_t header[4];
    if (!home_service_pack(&b, &s->snapshot)) {
        lmb_put32(header, (uint32_t)b.len); double deadline = nowd() + .2;
        if (!home_service_io(s->reply, header, sizeof header, 1, deadline))
            (void)home_service_io(s->reply, b.p, b.len, 1, deadline);
    }
    free(b.p); close(s->reply); s->reply = -1;
}

/* Only called with no live background threads or connected LAN sessions.
 * The grandchild owns its own session and closes every inherited descriptor
 * except its private singleton lock and local control socket. */
static int home_service_detach(HomeService *s, int inherited_fd) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child > 0) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0) { if (errno != EINTR) return -1; }
        /* Do not unlink a socket which now belongs to the keeper. */
        close(s->listener); close(s->lock); s->listener = s->lock = -1;
        return WIFEXITED(status) && !WEXITSTATUS(status) ? 1 : -1;
    }
    if (setsid() < 0) _exit(125);
    child = fork(); if (child < 0) _exit(125); if (child > 0) _exit(0);
    signal(SIGHUP, SIG_IGN);
    int limit = getdtablesize();
    for (int fd = 3; fd < limit; fd++) if (fd != s->listener && fd != s->lock && fd != inherited_fd) close(fd);
    char log[1200];
    if (home_service_path(s->snapshot.role, "log", log, sizeof log)) _exit(125);
    int nullfd = open("/dev/null", O_RDONLY), logfd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (nullfd < 0 || logfd < 0 || dup2(nullfd, 0) < 0 || dup2(logfd, 1) < 0 || dup2(logfd, 2) < 0) _exit(125);
    if (nullfd > 2) close(nullfd);
    if (logfd > 2) close(logfd);
    s->snapshot.pid = (uint64_t)getpid(); g_stopping = 0; g_tty = 0;
    install_chat_signal_handlers(); signal(SIGPIPE, SIG_IGN);
    return 0;
}

#endif /* native transport */

static int home_service_preparation_assign(HomeService *s,const char *name,const uint8_t allocation[32]) {
    if (strcmp(s->snapshot.role,"prepare") || !s->snapshot.preparation_results_known ||
        s->snapshot.preparation_result_count>=HOME_PREPARATION_RESULTS ||
        !name || !*name || strlen(name)>=sizeof s->snapshot.preparation_results[0].catalogue_name ||
        lmb_inventory_text(name)) return -1;
    unsigned nonzero=0; for (unsigned i=0;i<32;i++) nonzero|=allocation[i];
    if (!nonzero) return -1;
    for (uint32_t i=0;i<s->snapshot.preparation_result_count;i++)
        if (!memcmp(allocation,s->snapshot.preparation_results[i].allocation,32)) return -1;
    HomePreparationResult *r=&s->snapshot.preparation_results[s->snapshot.preparation_result_count];
    memset(r,0,sizeof *r); memcpy(r->allocation,allocation,32); strcpy(r->catalogue_name,name);
    s->snapshot.preparation_result_count++;
    if (!home_service_save(s)) return 0;
    /* Some persistence may have succeeded. Keep the ID as unconfirmed in
     * memory too; a later terminal snapshot must not erase that evidence. */
    return -1;
}

static int home_service_preparation_ready(HomeService *s,const uint8_t allocation[32]) {
    if (strcmp(s->snapshot.role,"prepare") || !s->snapshot.preparation_results_known) return -1;
    for (uint32_t i=0;i<s->snapshot.preparation_result_count;i++) {
        HomePreparationResult *r=&s->snapshot.preparation_results[i];
        if (memcmp(r->allocation,allocation,32)) continue;
        r->ready_observed=1;
        int rc=home_service_save(s);
        if (rc) r->ready_observed=0;
        return rc;
    }
    return -1;
}

static int home_service_ensure(void);
static HomeService *home_background_job;
static void home_service_preparation_detail(HomeService *job, const char *detail) {
    if (job->batch_total)
        snprintf(job->snapshot.detail, sizeof job->snapshot.detail, "%u/%u ready - %.450s",
            job->batch_completed, job->batch_total, detail);
    else snprintf(job->snapshot.detail, sizeof job->snapshot.detail, "%s", detail);
}
static int home_service_key(void) {
    if (!home_background_job) return -1;
    int op = home_service_poll(home_background_job);
    home_service_answer(home_background_job);
    return op == HOME_SVC_CANCEL || op == HOME_SVC_STOP ? 3 : -1;
}

static int home_service_foreground(void) {
    const char *v = getenv("LUMABRI_HOME_FOREGROUND");
    return v && !strcmp(v, "1"); /* explicit legacy lifecycle diagnostic */
}
#endif
