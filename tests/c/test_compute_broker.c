#define _DEFAULT_SOURCE
#define LMB_COMPUTE_BROKER_SERVER
#include "src/runtime/lumabri_compute_broker.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>

static void wait_count(_Atomic uint32_t *value, uint32_t expected) {
    uint64_t end = lmb_compute_now()+5000;
    while (atomic_load(value) != expected && lmb_compute_now() < end) usleep(1000);
    assert(atomic_load(value) == expected);
}

static _Atomic unsigned running, position;
static unsigned order[4];
static int raw_request(const char *path, const uint8_t request[8]) {
    struct sockaddr_un address = {.sun_family=AF_UNIX}; strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); assert(fd >= 0);
    assert(!connect(fd, (struct sockaddr *)&address, sizeof address));
    assert(write(fd, request, 8) == 8); return fd;
}
static void expect_closed(int fd) {
    struct pollfd p = {fd, POLLIN, 0}; char byte;
    assert(poll(&p, 1, 5000) > 0);
    ssize_t n = read(fd, &byte, 1);
    assert(n == 0 || (n < 0 && errno == ECONNRESET)); close(fd);
}
typedef struct { unsigned id; _Atomic int stop; int result; uint32_t timeout; } Work;
static int cancel(void *arg) { return atomic_load(&((Work *)arg)->stop); }
static void *run(void *arg) {
    Work *work = arg; int permit = -1;
    work->result = lmb_compute_acquire(&permit, work->timeout, cancel, work);
    if (!work->result) {
        assert(!atomic_fetch_add(&running, 1));
        unsigned at = atomic_fetch_add(&position, 1); assert(at < 4); order[at] = work->id;
        usleep(10000);
        assert(atomic_fetch_sub(&running, 1) == 1);
        lmb_compute_release(&permit);
    }
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "--hold")) {
        int lease;
        if (lmb_compute_acquire(&lease, 5000, NULL, NULL)) return 3;
        if (write(1, "ready", 5) != 5) return 4;
        for (;;) pause();
    }
    char directory[] = "/tmp/lmb-compute-test.XXXXXX", path[100];
    assert(mkdtemp(directory));
    assert(snprintf(path, sizeof path, "%s/sock", directory) < (int)sizeof path);
    assert(!setenv("LUMABRI_COMPUTE_SOCKET", path, 1));
    LmbComputeBroker broker;
    assert(!lmb_compute_broker_start(&broker, path));
    struct stat st; assert(!lstat(path, &st) && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);
    int held; assert(!lmb_compute_acquire(&held, 5000, NULL, NULL));
    wait_count(&broker.active, 1);
    pthread_t threads[3];
    Work works[3] = {{.id=1,.timeout=5000}, {.id=2,.timeout=5000}, {.id=3,.timeout=5000}};
    for (unsigned i = 0; i < 3; i++) {
        assert(!pthread_create(&threads[i], NULL, run, &works[i]));
        wait_count(&broker.queued, i+1);
    }
    lmb_compute_release(&held);
    for (unsigned i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL); assert(!works[i].result && order[i] == i+1);
    }
    wait_count(&broker.active, 0); wait_count(&broker.queued, 0);
    assert(!lmb_compute_acquire(&held, 5000, NULL, NULL));
    Work timeout = {.id=4,.timeout=100}, stopped = {.id=5,.timeout=5000};
    assert(!pthread_create(&threads[0], NULL, run, &timeout));
    pthread_join(threads[0], NULL); assert(timeout.result == -1);
    wait_count(&broker.queued, 0);
    assert(!pthread_create(&threads[0], NULL, run, &stopped));
    wait_count(&broker.queued, 1); atomic_store(&stopped.stop, 1);
    pthread_join(threads[0], NULL); assert(stopped.result == -1);
    wait_count(&broker.queued, 0);
    lmb_compute_release(&held); wait_count(&broker.active, 0);

    /* No stored PID or lease expiry is needed after a process crash. An
     * independently exec'd owner holds a descriptor until it actually dies. */
    int pipefd[2]; assert(!pipe(pipefd));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(pipefd[0]); assert(dup2(pipefd[1], 1) == 1); close(pipefd[1]);
        execl(argv[0], argv[0], "--hold", (char *)NULL); _exit(127);
    }
    close(pipefd[1]); char ready[5];
    struct pollfd p = {pipefd[0], POLLIN, 0};
    assert(poll(&p, 1, 5000) > 0 && read(pipefd[0], ready, 5) == 5 && !memcmp(ready, "ready", 5));
    close(pipefd[0]); wait_count(&broker.active, 1);
    assert(!kill(child, SIGKILL)); assert(waitpid(child, NULL, 0) == child);
    assert(!lmb_compute_acquire(&held, 5000, NULL, NULL));
    assert(atomic_load(&broker.grants) == 7);
    lmb_compute_release(&held); wait_count(&broker.active, 0);

    /* Malformed and partial requests do not occupy the active permit. */
    struct sockaddr_un address = {.sun_family=AF_UNIX}; strcpy(address.sun_path, path);
    int partial = socket(AF_UNIX, SOCK_STREAM, 0); assert(partial >= 0);
    assert(!connect(partial, (struct sockaddr *)&address, sizeof address));
    assert(write(partial, "L", 1) == 1);
    assert(!lmb_compute_acquire(&held, 5000, NULL, NULL));
    lmb_compute_release(&held);
    p = (struct pollfd){partial, POLLIN, 0}; assert(poll(&p, 1, 4000) > 0);
    assert(read(partial, ready, 1) == 0); close(partial);
    uint8_t request[8] = {'L','C','P',1,0x88,0x13,0,0}; /* 5000 ms */
    assert(lmb_compute_remaining(lmb_compute_now()+100, 100) == 0);
    assert(lmb_compute_remaining(lmb_compute_now(), 100) <= 100);
    assert(!lmb_compute_acquire(&held, 5000, NULL, NULL));
    for (unsigned variant = 0; variant < 3; variant++) {
        uint8_t bad[8]; memcpy(bad, request, sizeof bad);
        if (!variant) bad[0] = 'X';
        else if (variant == 1) memset(bad+4, 0, 4);
        else memset(bad+4, 255, 4);
        expect_closed(raw_request(path, bad));
        assert(atomic_load(&broker.active) == 1 && !atomic_load(&broker.queued));
    }
    int excess = raw_request(path, request);
    wait_count(&broker.queued, 1); assert(write(excess, "extra", 5) == 5);
    expect_closed(excess); wait_count(&broker.queued, 0);
    int full[LMB_COMPUTE_PENDING_MAX-1];
    for (unsigned i = 0; i < LMB_COMPUTE_PENDING_MAX-1; i++) {
        full[i] = raw_request(path, request); wait_count(&broker.queued, i+1);
    }
    int denied = -1; assert(lmb_compute_acquire(&denied, 500, NULL, NULL) == -1 && denied == -1);
    assert(atomic_load(&broker.active) == 1);
    for (unsigned i = 0; i < LMB_COMPUTE_PENDING_MAX-1; i++) close(full[i]);
    wait_count(&broker.queued, 0); lmb_compute_release(&held); wait_count(&broker.active, 0);
    assert(!lmb_compute_acquire(&held, 5000, NULL, NULL));
    Work waiting = {.id=6,.timeout=5000};
    assert(!pthread_create(&threads[0], NULL, run, &waiting)); wait_count(&broker.queued, 1);
    lmb_compute_broker_stop(&broker);
    pthread_join(threads[0], NULL); assert(waiting.result == -1);
    lmb_compute_release(&held);
    assert(lmb_compute_acquire(&held, 100, NULL, NULL) == -1 && held == -1);
    int file = open(path, O_WRONLY|O_CREAT|O_EXCL, 0600); assert(file >= 0); close(file);
    assert(lmb_compute_broker_start(&broker, path) == -1);
    assert(!lstat(path, &st) && S_ISREG(st.st_mode)); assert(!unlink(path));
    assert(!symlink("missing", path)); assert(lmb_compute_broker_start(&broker, path) == -1);
    assert(!lstat(path, &st) && S_ISLNK(st.st_mode)); assert(!unlink(path));
    assert(!unsetenv("LUMABRI_COMPUTE_SOCKET"));
    assert(!lmb_compute_acquire(&held, 100, NULL, NULL) && held == -1);
    assert(!rmdir(directory));
    puts("COMPUTE BROKER: PASS (private IPC, FIFO, one kernel, deadline, cancellation, process death, malformed/full queues, fail closed)");
    return 0;
}
