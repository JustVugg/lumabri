/* Private, versioned local control plane. A keeper owns its children and
 * leases; neither a terminal nor the replaceable manager owns the keeper.
 * Journal entries are observations, NEVER permission to replay an OFFER.
 * No persisted PID is ever used as authority to signal a process. */
#ifndef LUMABRI_SERVICE_H
#define LUMABRI_SERVICE_H
#include <sys/un.h>
#include <sys/file.h>

#define HOME_SVC_VERSION 1u
#define HOME_SVC_RECORD_VERSION 4u
#define HOME_SVC_MAX 65536u
enum { HOME_SVC_STATUS = 0, HOME_SVC_STOP, HOME_SVC_ACCEPT, HOME_SVC_DECLINE,
       HOME_SVC_UNLOAD, HOME_SVC_CANCEL, HOME_SVC_NEXT_MODEL };
enum { HOME_SVC_RUNNING = 1, HOME_SVC_DONE, HOME_SVC_FAILED, HOME_SVC_STOPPED };

typedef struct {
    uint8_t instance[32];
    uint32_t state, phase, retained, threads;
    uint64_t pid, segment_pid, host_pid, ram, revision;
    char role[16], name[64], tracker[256], detail[512];
    LmbHomeOffer offer;
    int has_offer;
    uint32_t model_count;
    uint64_t reserved_total;
    uint32_t compute_enabled, compute_active, compute_queued;
    uint64_t compute_grants;
    uint8_t allocation_set[32];
} HomeServiceSnapshot;

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

static int home_service_role(const char *role) {
    return !strcmp(role, "manager") || !strcmp(role, "donor") ||
           !strcmp(role, "tracker") || !strcmp(role, "prepare") || !strcmp(role, "api");
}

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

static int home_service_pack(LmbBuf *b, const HomeServiceSnapshot *s) {
    if (lmb_inventory_text(s->role) || lmb_inventory_text(s->name) ||
        lmb_inventory_text(s->tracker) || lmb_inventory_text(s->detail)) return -1;
    if (!home_service_role(s->role) || !s->state || s->state > HOME_SVC_STOPPED ||
        s->phase > LMB_HOME_CLOSED || s->retained > 1 ||
        (s->has_offer && !lmb_home_offer_valid(&s->offer))) return -1;
    if (s->model_count > 4 || s->reserved_total > s->ram) return -1;
    if (s->compute_enabled > 1 || s->compute_active > 1 || s->compute_queued > 32 ||
        (!s->compute_enabled && (s->compute_active || s->compute_queued))) return -1;
    if (lmb_buf_u32(b, HOME_SVC_RECORD_VERSION) || lmb_buf_bytes(b, s->instance, 32) ||
        lmb_buf_u32(b, s->state) || lmb_buf_u32(b, s->phase) ||
        lmb_buf_u32(b, s->retained) || lmb_buf_u32(b, s->threads) ||
        lmb_buf_u64(b, s->pid) || lmb_buf_u64(b, s->segment_pid) ||
        lmb_buf_u64(b, s->host_pid) || lmb_buf_u64(b, s->ram) || lmb_buf_u64(b, s->revision) ||
        lmb_buf_str(b, s->role) || lmb_buf_str(b, s->name) ||
        lmb_buf_str(b, s->tracker) || lmb_buf_str(b, s->detail) ||
        lmb_buf_u32(b, (uint32_t)s->has_offer)) return -1;
    LmbBuf offer = {0};
    int rc = s->has_offer ? lmb_home_offer_pack(&offer, &s->offer) : 0;
    if (!rc) rc = lmb_buf_u32(b, (uint32_t)offer.len) || (offer.len && lmb_buf_bytes(b, offer.p, offer.len));
    free(offer.p);
    if (!rc) rc = lmb_buf_u32(b, s->model_count) || lmb_buf_u64(b, s->reserved_total);
    if (!rc) rc = lmb_buf_u32(b, s->compute_enabled) || lmb_buf_u32(b, s->compute_active) ||
        lmb_buf_u32(b, s->compute_queued) || lmb_buf_u64(b, s->compute_grants);
    if (!rc) rc = lmb_buf_bytes(b, s->allocation_set, 32);
    return rc || b->len > HOME_SVC_MAX ? -1 : 0;
}

static int home_service_unpack(const uint8_t *bytes, size_t n, HomeServiceSnapshot *s) {
    memset(s, 0, sizeof *s);
    LmbCur c = {bytes, n, 0}; uint32_t v, has, size;
    if (lmb_cur_u32(&c, &v) || v < 1 || v > HOME_SVC_RECORD_VERSION || c.len - c.off < 32) return -1;
    memcpy(s->instance, c.p + c.off, 32); c.off += 32;
    if (lmb_cur_u32(&c, &s->state) || lmb_cur_u32(&c, &s->phase) ||
        lmb_cur_u32(&c, &s->retained) || lmb_cur_u32(&c, &s->threads) ||
        lmb_cur_u64(&c, &s->pid) || lmb_cur_u64(&c, &s->segment_pid) ||
        lmb_cur_u64(&c, &s->host_pid) || lmb_cur_u64(&c, &s->ram) || lmb_cur_u64(&c, &s->revision) ||
        lmb_inventory_string(&c, s->role, sizeof s->role) ||
        lmb_inventory_string(&c, s->name, sizeof s->name) ||
        lmb_inventory_string(&c, s->tracker, sizeof s->tracker) ||
        lmb_inventory_string(&c, s->detail, sizeof s->detail) ||
        lmb_cur_u32(&c, &has) || has > 1 || lmb_cur_u32(&c, &size) || size > c.len - c.off) return -1;
    s->has_offer = (int)has;
    LmbCur offer = {c.p + c.off, size, 0};
    if ((has && lmb_home_offer_unpack(&offer, &s->offer)) || (!has && size)) return -1;
    c.off += size;
    if (v >= 2 && (lmb_cur_u32(&c, &s->model_count) || lmb_cur_u64(&c, &s->reserved_total))) return -1;
    if (v >= 3 && (lmb_cur_u32(&c, &s->compute_enabled) || lmb_cur_u32(&c, &s->compute_active) ||
        lmb_cur_u32(&c, &s->compute_queued) || lmb_cur_u64(&c, &s->compute_grants))) return -1;
    if (v >= 4) {
        if (c.off > c.len || c.len-c.off < 32) return -1;
        memcpy(s->allocation_set, c.p+c.off, 32); c.off += 32;
    }
    if (c.off != c.len) return -1;
    LmbBuf check = {0}; int rc = home_service_pack(&check, s); free(check.p); return rc;
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
    uint8_t request[80] = {0};
    lmb_put32(request, HOME_SVC_VERSION); lmb_put32(request + 4, op);
    if (expected) {
        memcpy(request + 8, expected->instance, 32);
        memcpy(request + 40, expected->offer.id, 32);
        lmb_put32(request + 72, (uint32_t)expected->revision);
        lmb_put32(request + 76, (uint32_t)(expected->revision >> 32));
    }
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
        home_service_io(fd, r, sizeof r, 0, nowd() + .2) ||
        lmb_get32(r) != HOME_SVC_VERSION || lmb_get32(r + 4) > HOME_SVC_NEXT_MODEL) { close(fd); return -1; }
    uint32_t op = lmb_get32(r + 4);
    uint64_t revision = (uint64_t)lmb_get32(r + 72) | ((uint64_t)lmb_get32(r + 76) << 32);
    if (op && (memcmp(r + 8, s->snapshot.instance, 32) || memcmp(r + 40, s->snapshot.offer.id, 32) || revision != s->snapshot.revision)) {
        close(fd); return -1;
    }
    s->reply = fd; return (int)op;
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
