/* Query the installed executable, never infer its capabilities from the TUI
 * compiler or the presence of a GPU/OpenMP library on disk. No model is opened. */
#ifndef LUMABRI_RUNTIME_PROBE_H
#define LUMABRI_RUNTIME_PROBE_H
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int lmb_runtime_thread_capacity(const char *binary, unsigned *capacity) {
    int fds[2];
    *capacity = 0;
    if (pipe(fds)) return -1;
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pid_t child = fork();
    if (!child) {
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[0]); close(fds[1]);
        execl(binary, binary, "--thread-capacity", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) { close(fds[0]); return -1; }
    char reply[32] = {0}; size_t used = 0;
    int status = 0, finished = 0, bad = 0;
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    /* Both output and process exit are bounded, including a broken binary
     * which emits a plausible answer and then hangs. */
    for (int tick = 0; tick < 200; tick++) {
        ssize_t n = read(fds[0], reply + used, sizeof reply - 1 - used);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) bad = 1;
        if (used == sizeof reply - 1) bad = 1;
        pid_t rc = waitpid(child, &status, WNOHANG);
        if (rc == child) {
            finished = 1;
            n = read(fds[0], reply + used, sizeof reply - 1 - used);
            if (n > 0) used += (size_t)n;
            break;
        }
        if (bad || (rc < 0 && errno != EINTR)) break;
        (void)poll(NULL, 0, 10);
    }
    close(fds[0]);
    if (!finished) {
        (void)kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
        return -1;
    }
    if (bad || !WIFEXITED(status) || WEXITSTATUS(status) || !used) return -1;
    reply[used] = 0;
    for (size_t i = 0; i + 1 < used; i++) if (reply[i] < '0' || reply[i] > '9') return -1;
    if (reply[used - 1] != '\n') return -1;
    char *end; unsigned long value = strtoul(reply, &end, 10);
    if (end != reply + used - 1 || !value || value > 256) return -1;
    *capacity = (unsigned)value;
    return 0;
}
#endif
