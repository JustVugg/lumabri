#define _GNU_SOURCE
#include "lumabri_ready.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "child")) {
        int read_fd = atoi(argv[2]), write_fd = atoi(argv[3]);
        errno = 0;
        assert(fcntl(read_fd, F_GETFD) == -1 && errno == EBADF);
        assert(fcntl(write_fd, F_GETFD) >= 0);
        assert(!lmb_ready_notify());
        return 0;
    }
    int fd[2];
    assert(!lmb_ready_pipe(fd));
    assert(fcntl(fd[0], F_GETFD) & FD_CLOEXEC);
    assert(fcntl(fd[1], F_GETFD) & FD_CLOEXEC);
    /* Only the explicitly designated writer survives the engine exec. */
    assert(!fcntl(fd[1], F_SETFD, 0));
    char read_fd[32], write_fd[32];
    snprintf(read_fd, sizeof read_fd, "%d", fd[0]);
    snprintf(write_fd, sizeof write_fd, "%d", fd[1]);
    assert(!setenv("LUMABRI_READY_FD", write_fd, 1));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        execl(argv[0], argv[0], "child", read_fd, write_fd, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    char byte = 0;
    assert(read(fd[0], &byte, 1) == 1 && byte == 'R');
    assert(read(fd[0], &byte, 1) == 0);
    close(fd[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("READY PIPE: PASS (CLOEXEC, explicit writer inheritance, readiness, EOF)");
    return 0;
}
