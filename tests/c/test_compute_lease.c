#define _GNU_SOURCE
#include "lumabri_machine.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char home[] = "/tmp/lumabri-compute-lease.XXXXXX";
    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);

    char owner[256];
    int first = lmb_machine_compute_lease_acquire(
        "DeepSeek-V4-Flash", "tracker.example:7300", owner, sizeof owner);
    assert(first >= 0);

    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        /* Drop the inherited reference; the parent still owns the same open
         * file description, so a fresh contender must fail non-blocking. */
        close(first);
        int other = lmb_machine_compute_lease_acquire(
            "DeepSeek-V4-Flash", "tracker.example:7300", owner, sizeof owner);
        if (other >= 0 || (errno != EWOULDBLOCK && errno != EAGAIN) ||
            !strstr(owner, "DeepSeek-V4-Flash")) _exit(2);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* The spawned executor inherits the lease. If its parent disappears,
     * the lock therefore remains held until that executor actually exits. */
    pid_t keeper = fork();
    assert(keeper >= 0);
    if (!keeper) {
        for (;;) pause();
    }
    close(first);
    int blocked = lmb_machine_compute_lease_acquire(
        "another-model", "tracker.example:7300", owner, sizeof owner);
    assert(blocked < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
    assert(kill(keeper, SIGTERM) == 0);
    assert(waitpid(keeper, &status, 0) == keeper);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    int replacement = lmb_machine_compute_lease_acquire(
        "another-model", "tracker.example:7300", owner, sizeof owner);
    assert(replacement >= 0);
    close(replacement);

    char lock[1200], directory[1100];
    snprintf(directory, sizeof directory, "%s/.lumabri", home);
    snprintf(lock, sizeof lock, "%s/compute-donor.lock", directory);
    assert(unlink(lock) == 0);
    assert(rmdir(directory) == 0);
    assert(rmdir(home) == 0);
    puts("COMPUTE LEASE TEST: PASS (one RAM owner; crash-safe release)");
    return 0;
}
