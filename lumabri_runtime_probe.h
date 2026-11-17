/* Query the installed executable, never infer its capabilities from the TUI
 * compiler or the presence of a GPU/OpenMP library on disk. No model is opened. */
#ifndef LUMABRI_RUNTIME_PROBE_H
#define LUMABRI_RUNTIME_PROBE_H
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "src/runtime/lumabri_process_probe_windows.h"
#else
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* One bounded question to the installed executable: a single option, one
 * short line back. `reply` holds 32 bytes. */
static int lmb_runtime_probe_line(const char *binary, const char *option, char *reply, size_t *length) {
    size_t used = 0;
    memset(reply, 0, 32);
#ifdef _WIN32
    const char *args[] = {option}; DWORD status;
    if (lmb_windows_probe_capture(binary, args, 1, reply, 32, &used, &status, 2000) ||
        status || !used) return -1;
    if (used >= 2 && reply[used - 2] == '\r' && reply[used - 1] == '\n') {
        used--; reply[used - 1] = '\n';
    }
#else
    int fds[2];
    if (pipe(fds)) return -1;
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pid_t child = fork();
    if (!child) {
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[0]); close(fds[1]);
        execl(binary, binary, option, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) { close(fds[0]); return -1; }
    int status = 0, finished = 0, bad = 0;
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    /* Both output and process exit are bounded, including a broken binary
     * which emits a plausible answer and then hangs. */
    for (int tick = 0; tick < 200; tick++) {
        ssize_t n = read(fds[0], reply + used, 31 - used);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) bad = 1;
        if (used == 31) bad = 1;
        pid_t rc = waitpid(child, &status, WNOHANG);
        if (rc == child) {
            finished = 1;
            n = read(fds[0], reply + used, 31 - used);
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
#endif
    reply[used] = 0;
    if (reply[used - 1] != '\n') return -1;
    *length = used;
    return 0;
}

static int lmb_runtime_thread_capacity(const char *binary, unsigned *capacity) {
    if (!capacity) { errno = EINVAL; return -1; }
    *capacity = 0;
    char reply[32]; size_t used = 0;
    if (lmb_runtime_probe_line(binary, "--thread-capacity", reply, &used)) return -1;
    for (size_t i = 0; i + 1 < used; i++) if (reply[i] < '0' || reply[i] > '9') return -1;
    char *end; unsigned long value = strtoul(reply, &end, 10);
    if (end != reply + used - 1 || !value || value > 256) return -1;
    *capacity = (unsigned)value;
    return 0;
}

/* 1 when the installed Segment runtime links the CUDA tier. A runtime that
 * does not know the question, or answers anything else, is CPU only. */
static inline int lmb_runtime_has_cuda(const char *binary) {
    char reply[32]; size_t used = 0;
    return !lmb_runtime_probe_line(binary, "--backends", reply, &used) && !strcmp(reply, "cpu cuda\n");
}
#endif
