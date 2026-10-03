/* Private per-donor compute permits. Resident allocations keep their memory;
 * only a local kernel owns the permit. Never hold it around a network wait.
 * The connected descriptor owns the grant: exit/disconnect releases it.
 * Same-UID IPC is not a sandbox against arbitrary programs of that user. */
#ifndef LUMABRI_COMPUTE_BROKER_H
#define LUMABRI_COMPUTE_BROKER_H
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LMB_COMPUTE_PENDING_MAX 32

static inline uint64_t lmb_compute_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return UINT64_MAX;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static inline int lmb_compute_peer(int fd) {
#ifdef __APPLE__
    uid_t uid; gid_t gid;
    return getpeereid(fd, &uid, &gid) || uid != geteuid() ? -1 : 0;
#elif defined(__linux__)
    struct { pid_t pid; uid_t uid; gid_t gid; } cred;
    socklen_t n = sizeof cred;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &n) ||
        n != sizeof cred || cred.uid != geteuid() ? -1 : 0;
#else
    (void)fd; errno = ENOTSUP; return -1;
#endif
}

static inline int lmb_compute_fd(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, flags | O_NONBLOCK) ? -1 : 0;
}

/* Share the same wait budget with a preceding in-process queue. */
static inline uint32_t lmb_compute_remaining(uint64_t began, uint32_t budget) {
    uint64_t now = lmb_compute_now();
    if (now < began || now-began >= budget) return 0;
    return budget-(uint32_t)(now-began);
}

/* Success sets a live permit fd, or -1 for unmanaged CLI diagnostics. A
 * configured but dead broker NEVER silently falls back to unbounded compute. */
static inline int lmb_compute_acquire(int *permit, uint32_t wait_ms,
                                      int (*cancel)(void *), void *opaque) {
    *permit = -1;
    const char *path = getenv("LUMABRI_COMPUTE_SOCKET");
    if (!path || !*path) return 0;
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    if (strlen(path) >= sizeof address.sun_path || !wait_ms || wait_ms > 300000) {
        errno = EINVAL; return -1;
    }
    memcpy(address.sun_path, path, strlen(path)+1);
    uint64_t start = lmb_compute_now();
    if (start > UINT64_MAX-wait_ms) return -1;
    uint64_t deadline = start+wait_ms;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
    int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (lmb_compute_fd(fd)) { close(fd); return -1; }
    int connected = connect(fd, (struct sockaddr *)&address, sizeof address);
    while (connected && errno == EINPROGRESS) {
        if (cancel && cancel(opaque)) { errno = ECANCELED; break; }
        uint32_t left = lmb_compute_remaining(start, wait_ms);
        if (!left) { errno = ETIMEDOUT; break; }
        struct pollfd ready = {fd, POLLOUT, 0};
        int rc = poll(&ready, 1, left < 100 ? (int)left : 100);
        if (rc < 0 && errno != EINTR) break;
        if (rc <= 0) { errno = EINPROGRESS; continue; }
        int error = 0; socklen_t n = sizeof error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &n)) break;
        if (error) { errno = error; break; }
        connected = 0;
    }
    if (connected || lmb_compute_peer(fd)) { int error = errno; close(fd); errno = error; return -1; }
    uint8_t request[8] = {'L','C','P',1};
    for (unsigned i = 0; i < 4; i++) request[4+i] = (uint8_t)(wait_ms >> (8*i));
    size_t sent = 0;
    for (;;) {
        if (cancel && cancel(opaque)) { errno = ECANCELED; break; }
        uint64_t now = lmb_compute_now();
        if (now >= deadline) { errno = ETIMEDOUT; break; }
        uint64_t remaining = deadline-now;
        struct pollfd ready = {fd, sent < sizeof request ? POLLOUT : POLLIN, 0};
        int rc = poll(&ready, 1, remaining < 100 ? (int)remaining : 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || (ready.revents & (POLLHUP|POLLERR|POLLNVAL))) break;
        if (!rc) continue;
        if (sent < sizeof request) {
#ifdef MSG_NOSIGNAL
            ssize_t n = send(fd, request+sent, sizeof request-sent, MSG_NOSIGNAL);
#else
            ssize_t n = send(fd, request+sent, sizeof request-sent, 0);
#endif
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) break;
            sent += (size_t)n;
        } else {
            uint8_t grant = 0; ssize_t n = recv(fd, &grant, 1, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n != 1 || grant != 1) break;
            if (cancel && cancel(opaque)) { errno = ECANCELED; break; }
            *permit = fd; return 0;
        }
    }
    int error = errno ? errno : ECONNRESET; close(fd); errno = error; return -1;
}

static inline void lmb_compute_release(int *permit) {
    if (*permit >= 0) close(*permit);
    *permit = -1;
}

#ifdef LMB_COMPUTE_BROKER_SERVER
typedef struct {
    int fd, state;
    uint8_t request[8];
    size_t received;
    uint64_t ticket, deadline;
} LmbComputeClient;

typedef struct {
    int listener, started;
    pthread_t thread;
    _Atomic int stop;
    _Atomic uint32_t queued, active;
    _Atomic uint64_t grants;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} LmbComputeBroker;

static inline void lmb_compute_drop(LmbComputeClient *client) {
    if (client->fd >= 0) close(client->fd);
    memset(client, 0, sizeof *client); client->fd = -1;
}

static void *lmb_compute_serve(void *opaque) {
    LmbComputeBroker *broker = opaque;
    LmbComputeClient clients[LMB_COMPUTE_PENDING_MAX];
    memset(clients, 0, sizeof clients);
    for (unsigned i = 0; i < LMB_COMPUTE_PENDING_MAX; i++) clients[i].fd = -1;
    uint64_t ticket = 0;
    while (!atomic_load(&broker->stop)) {
        struct pollfd pending[LMB_COMPUTE_PENDING_MAX+1] = {{broker->listener, POLLIN, 0}};
        for (unsigned i = 0; i < LMB_COMPUTE_PENDING_MAX; i++) pending[i+1] = (struct pollfd){clients[i].fd, POLLIN, 0};
        int rc = poll(pending, LMB_COMPUTE_PENDING_MAX+1, 50);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || pending[0].revents & (POLLERR|POLLHUP|POLLNVAL)) break;
        uint64_t now = lmb_compute_now();
        if (pending[0].revents & POLLIN) {
            int fd = accept(broker->listener, NULL, NULL);
            if (fd >= 0) {
#ifdef SO_NOSIGPIPE
                int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                if (!lmb_compute_fd(fd) && !lmb_compute_peer(fd))
                    for (unsigned i = 0; i < LMB_COMPUTE_PENDING_MAX; i++) if (clients[i].fd < 0) {
                        clients[i].fd = fd; clients[i].deadline = now+2000; fd = -1; break;
                    }
                if (fd >= 0) close(fd);
            }
        }
        int active = 0, first = -1;
        uint32_t queued = 0;
        for (unsigned i = 0; i < LMB_COMPUTE_PENDING_MAX; i++) {
            LmbComputeClient *c = &clients[i];
            if (c->fd < 0) continue;
            short events = pending[i+1].fd == c->fd ? pending[i+1].revents : 0;
            if ((events & (POLLHUP|POLLERR|POLLNVAL)) || (c->state != 2 && now >= c->deadline)) {
                lmb_compute_drop(c); continue;
            }
            if (events & POLLIN) {
                if (c->state) { lmb_compute_drop(c); continue; } /* EOF, or extra bytes */
                ssize_t n = recv(c->fd, c->request+c->received, sizeof c->request-c->received, 0);
                if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                if (n <= 0) { lmb_compute_drop(c); continue; }
                c->received += (size_t)n;
                if (c->received == sizeof c->request) {
                    uint32_t wait = 0;
                    for (unsigned j = 0; j < 4; j++) wait |= (uint32_t)c->request[4+j] << (8*j);
                    if (memcmp(c->request, "LCP\1", 4) || !wait || wait > 300000 || ticket == UINT64_MAX) {
                        lmb_compute_drop(c); continue;
                    }
                    c->state = 1; c->ticket = ++ticket; c->deadline = now+wait;
                }
            }
            if (c->state == 2) active = 1;
            if (c->state == 1) {
                queued++;
                if (first < 0 || c->ticket < clients[first].ticket) first = (int)i;
            }
        }
        if (!active && first >= 0) {
            LmbComputeClient *c = &clients[first];
            const uint8_t grant = 1;
#ifdef MSG_NOSIGNAL
            ssize_t n = send(c->fd, &grant, 1, MSG_NOSIGNAL);
#else
            ssize_t n = send(c->fd, &grant, 1, 0);
#endif
            queued--;
            if (n == 1) { c->state = 2; active = 1; atomic_fetch_add(&broker->grants, 1); }
            else lmb_compute_drop(c);
        }
        atomic_store(&broker->queued, queued); atomic_store(&broker->active, (uint32_t)active);
    }
    for (unsigned i = 0; i < LMB_COMPUTE_PENDING_MAX; i++) lmb_compute_drop(&clients[i]);
    atomic_store(&broker->queued, 0); atomic_store(&broker->active, 0);
    atomic_store(&broker->stop, 1);
    return NULL;
}

/* Caller holds the donor singleton lock and supplies a private owned parent
 * directory. A stale socket may be removed, never another file or symlink. */
static inline int lmb_compute_broker_start(LmbComputeBroker *broker, const char *path) {
    memset(broker, 0, sizeof *broker); broker->listener = -1;
    if (!path || !*path || strlen(path) >= sizeof broker->path) return -1;
    struct stat st;
    if (!lstat(path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) || unlink(path)) return -1;
    } else if (errno != ENOENT) return -1;
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    memcpy(address.sun_path, path, strlen(path)+1);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (lmb_compute_fd(fd) || bind(fd, (struct sockaddr *)&address, sizeof address)) { close(fd); return -1; }
    if (chmod(path, 0600) || listen(fd, LMB_COMPUTE_PENDING_MAX)) { close(fd); unlink(path); return -1; }
    broker->listener = fd; memcpy(broker->path, path, strlen(path)+1);
    if (pthread_create(&broker->thread, NULL, lmb_compute_serve, broker)) {
        close(fd); broker->listener = -1; unlink(path); return -1;
    }
    broker->started = 1; return 0;
}

static inline void lmb_compute_broker_stop(LmbComputeBroker *broker) {
    if (!broker->started) return;
    atomic_store(&broker->stop, 1); pthread_join(broker->thread, NULL);
    close(broker->listener); unlink(broker->path); broker->listener = -1; broker->started = 0;
}
#endif
#endif
