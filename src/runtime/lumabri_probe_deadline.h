/* A bounded optional measurement owns this timer, not the engine descriptor.
 * Stop/join it before closing or reusing the descriptor. Shutdown wakes a
 * stalled Hosted read without asynchronous close/PID tricks. */
#ifndef LUMABRI_PROBE_DEADLINE_H
#define LUMABRI_PROBE_DEADLINE_H
#include <math.h>
#include "src/protocol/lumabri_socket.h"
#include <pthread.h>
#include <stdatomic.h>

#include <time.h>

#define LMB_QUICK_PROBE_TOKENS 8u
#define LMB_QUICK_PROBE_SECONDS 20.0

typedef struct {
    pthread_t thread;
    atomic_int stop, expired;
    int started;
    LmbSocket fd;
    double until;
} LmbProbeDeadline;

static inline double lmb_probe_now(void) {
#ifdef _WIN32
    return GetTickCount64()/1000.;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}
static void *lmb_probe_watch(void *arg) {
    LmbProbeDeadline *p = arg;
    while (!atomic_load(&p->stop)) {
        double now = lmb_probe_now();
        if (now < 0 || now >= p->until) {
            atomic_store(&p->expired, 1);
#ifdef _WIN32
            (void)shutdown(p->fd,SD_BOTH);
#else
            (void)shutdown(p->fd,SHUT_RDWR);
#endif
            break;
        }
#ifdef _WIN32
        Sleep(10);
#else
        (void)poll(NULL,0,10);
#endif
    }
    return NULL;
}
static inline int lmb_probe_start(LmbProbeDeadline *p, LmbSocket fd, double seconds) {
    if (!p || p->started || fd == LMB_INVALID_SOCKET || !isfinite(seconds) || seconds <= 0 || seconds > 60) return -1;
    double now = lmb_probe_now();
    if (now < 0) return -1;
    p->fd = fd; p->until = now + seconds;
    atomic_init(&p->stop, 0); atomic_init(&p->expired, 0);
    if (pthread_create(&p->thread, NULL, lmb_probe_watch, p)) return -1;
    p->started = 1; return 0;
}
static inline int lmb_probe_stop(LmbProbeDeadline *p) {
    if (!p || !p->started) return 0;
    double now = lmb_probe_now();
    atomic_store(&p->stop, 1);
    pthread_join(p->thread, NULL); p->started = 0;
    return atomic_load(&p->expired) || now < 0 || now >= p->until;
}
#endif
