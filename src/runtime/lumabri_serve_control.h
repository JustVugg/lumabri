/* A single in-flight codec request, with cooperative cancellation read on a
 * separate thread. Only the host's multiplexed gateway enables this path:
 * it never pipelines SUBMITs and waits for DONE/ERROR before RESET_SLOT.
 * The reader never calls Colibri or writes stdout. */
#ifndef LMB_SERVE_CONTROL_H
#define LMB_SERVE_CONTROL_H
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lumabri_session_limits.h"

typedef struct {
    _Atomic int stop, cancelled, invalid;
    _Atomic unsigned received;
    unsigned request_id;
    pthread_t reader;
    int started;
} LmbServeControl;

static void *lmb_serve_control_reader(void *opaque) {
    LmbServeControl *c = opaque;
    char line[128]; size_t length = 0;
    struct timespec began = {0};
    while (!atomic_load(&c->stop) || length) {
        if (length) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if ((double)(now.tv_sec-began.tv_sec) + (now.tv_nsec-began.tv_nsec)/1e9 >= 1.0) break;
        }
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&p, 1, 25);
        if (ready < 0 && errno == EINTR) continue;
        if (!ready) continue;
        if (ready < 0 || !(p.revents & POLLIN)) break;
        char ch;
        ssize_t got = read(STDIN_FILENO, &ch, 1);
        if (got < 0 && errno == EINTR) continue;
        if (got != 1 || length >= sizeof line-1) break;
        atomic_fetch_add(&c->received, 1);
        if (ch != '\n') {
            if (!length) clock_gettime(CLOCK_MONOTONIC, &began);
            line[length++] = ch; continue;
        }
        line[length] = 0;
        unsigned id; char extra;
        if ((sscanf(line, "CANCEL %u %c", &id, &extra) != 1 &&
             sscanf(line, "STOP %u %c", &id, &extra) != 1) || id != c->request_id) break;
        atomic_store(&c->cancelled, 1);
        length = 0;
    }
    if (!atomic_load(&c->stop) || length) {
        atomic_store(&c->invalid, 1);
        atomic_store(&c->cancelled, 1);
    }
    return NULL;
}

static int lmb_serve_control_start(LmbServeControl *c, unsigned id) {
    memset(c, 0, sizeof *c); c->request_id = id;
    if (pthread_create(&c->reader, NULL, lmb_serve_control_reader, c)) return -1;
    c->started = 1; return 0;
}

static void lmb_serve_control_stop(LmbServeControl *c) {
    if (!c->started) return;
    atomic_store(&c->stop, 1);
    pthread_join(c->reader, NULL); c->started = 0;
}
#endif
