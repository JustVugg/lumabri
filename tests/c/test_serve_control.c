#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "src/runtime/lumabri_serve_control.h"
#include <assert.h>
#include <sys/wait.h>

static void child_test(unsigned mode) {
    int pipes[2]; assert(!pipe(pipes));
    assert(dup2(pipes[0], STDIN_FILENO) == STDIN_FILENO); close(pipes[0]);
    LmbServeControl c;
    assert(!lmb_serve_control_start(&c, 17));
    if (mode == 0) {
        lmb_serve_control_stop(&c);
        assert(!atomic_load(&c.cancelled) && !atomic_load(&c.invalid));
    } else if (mode == 1) {
        assert(write(pipes[1], "CANCEL 17\n", 10) == 10);
        for (int i = 0; i < 5000 && !atomic_load(&c.cancelled); i++) usleep(1000);
        assert(atomic_load(&c.cancelled)); lmb_serve_control_stop(&c);
        assert(!atomic_load(&c.invalid));
    } else if (mode == 2) {
        assert(write(pipes[1], "CANCEL 18\n", 10) == 10);
        for (int i = 0; i < 5000 && !atomic_load(&c.invalid); i++) usleep(1000);
        assert(atomic_load(&c.cancelled) && atomic_load(&c.invalid));
        lmb_serve_control_stop(&c);
    } else if (mode == 3) {
        assert(write(pipes[1], "CANCEL 1", 8) == 8);
        for (int i = 0; i < 5000 && atomic_load(&c.received) < 8; i++) usleep(1000);
        assert(atomic_load(&c.received) == 8);
        atomic_store(&c.stop, 1);
        assert(write(pipes[1], "7\n", 2) == 2);
        lmb_serve_control_stop(&c);
        assert(atomic_load(&c.cancelled) && !atomic_load(&c.invalid));
    } else {
        assert(write(pipes[1], "CANCEL", 6) == 6);
        for (int i = 0; i < 5000 && atomic_load(&c.received) < 6; i++) usleep(1000);
        assert(atomic_load(&c.received) == 6);
        lmb_serve_control_stop(&c);
        assert(atomic_load(&c.cancelled) && atomic_load(&c.invalid));
    }
    close(pipes[1]);
}

int main(void) {
    for (unsigned mode = 0; mode < 5; mode++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { child_test(mode); _exit(0); }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("SERVE CONTROL: PASS (idle, cancellation, wrong request, partial-frame stop race and deadline)");
    return 0;
}
