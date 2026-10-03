#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "lumabri_run_gate.h"

#include <errno.h>
#include <string.h>
#include <time.h>

struct LmbRunWaiter {
    struct LmbRunWaiter *next;
    int result; /* 0 waiting, 1 admitted, -1 cancelled, 2 expired */
    uint64_t deadline_ms;
    LmbRunCancelFn cancel;
    void *opaque;
};

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void publish(LmbRunGate *gate) {
    atomic_store(&gate->published_in_use, gate->in_use);
    atomic_store(&gate->published_queued, gate->queued);
}

static void admit(LmbRunGate *gate) {
    while (gate->in_use < gate->capacity && gate->head) {
        LmbRunWaiter *waiter = gate->head;
        gate->head = waiter->next;
        if (!gate->head) gate->tail = NULL;
        gate->queued--;
        /* The waiter may not have been scheduled to observe its timeout or
         * cancellation yet. Never hand that expired request a fresh permit. */
        if (waiter->cancel && waiter->cancel(waiter->opaque)) waiter->result = -1;
        else if (monotonic_ms() >= waiter->deadline_ms) waiter->result = 2;
        else { gate->in_use++; waiter->result = 1; }
    }
    publish(gate);
    pthread_cond_broadcast(&gate->changed);
}

int lmb_run_gate_init(LmbRunGate *gate, uint32_t capacity,
                      uint32_t max_queue) {
    if (!gate || !capacity) return -1;
    memset(gate, 0, sizeof *gate);
    gate->capacity = capacity;
    gate->max_queue = max_queue;
    if (pthread_mutex_init(&gate->lock, NULL)) return -1;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) {
        pthread_mutex_destroy(&gate->lock); return -1;
    }
    int failed = 0;
#ifndef __APPLE__
    failed = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    if (!failed) failed = pthread_cond_init(&gate->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (failed) { pthread_mutex_destroy(&gate->lock); return -1; }
    publish(gate);
    return 0;
}

void lmb_run_gate_destroy(LmbRunGate *gate) {
    if (!gate) return;
    pthread_cond_destroy(&gate->changed);
    pthread_mutex_destroy(&gate->lock);
}

static void waiter_remove(LmbRunGate *gate, LmbRunWaiter *waiter) {
    LmbRunWaiter *previous = NULL;
    for (LmbRunWaiter *item = gate->head; item; item = item->next) {
        if (item != waiter) { previous = item; continue; }
        if (previous) previous->next = item->next;
        else gate->head = item->next;
        if (gate->tail == item) gate->tail = previous;
        gate->queued--;
        break;
    }
    admit(gate);
}

int lmb_run_gate_enter(LmbRunGate *gate, uint32_t wait_ms,
                       LmbRunCancelFn cancel, void *cancel_opaque) {
    if (!gate || !wait_ms) return 0;
    uint64_t started = monotonic_ms();
    if (started == UINT64_MAX || started > UINT64_MAX - wait_ms) return 0;
    uint64_t deadline = started + wait_ms;
    pthread_mutex_lock(&gate->lock);
    int stopped = cancel && cancel(cancel_opaque);
    if (stopped || monotonic_ms() >= deadline) {
        int result = stopped ? -1 : 0;
        pthread_mutex_unlock(&gate->lock); return result;
    }
    if (!gate->head && gate->in_use < gate->capacity) {
        gate->in_use++;
        publish(gate);
        pthread_mutex_unlock(&gate->lock);
        return 1;
    }
    if (gate->queued >= gate->max_queue) {
        pthread_mutex_unlock(&gate->lock);
        return 0;
    }
    LmbRunWaiter waiter = {.deadline_ms=deadline, .cancel=cancel, .opaque=cancel_opaque};
    if (gate->tail) gate->tail->next = &waiter;
    else gate->head = &waiter;
    gate->tail = &waiter;
    gate->queued++;
    publish(gate);

    int result = 0;
    while (!waiter.result) {
        if (cancel && cancel(cancel_opaque)) { result = -1; break; }
        uint64_t now = monotonic_ms();
        if (now >= deadline) break;
        uint64_t tick = deadline - now < 100u ? deadline - now : 100u;
#ifdef __APPLE__
        /* Darwin lacks pthread_condattr_setclock; the relative wait also
         * stays independent of wall-clock/NTP adjustments. */
        struct timespec relative = {0, (long)tick * 1000000L};
        int rc = pthread_cond_timedwait_relative_np(&gate->changed, &gate->lock, &relative);
#else
        uint64_t until = now + tick;
        struct timespec poll_deadline = {(time_t)(until / 1000u), (long)(until % 1000u) * 1000000L};
        int rc = pthread_cond_timedwait(&gate->changed, &gate->lock,
                                        &poll_deadline);
#endif
        if (rc && rc != ETIMEDOUT) break;
    }
    if (waiter.result == 1) {
        result = 1;
        if (cancel && cancel(cancel_opaque)) {
            gate->in_use--; admit(gate); result = -1;
        }
    } else if (waiter.result) result = waiter.result == -1 ? -1 : 0;
    else waiter_remove(gate, &waiter);
    pthread_mutex_unlock(&gate->lock);
    return result;
}

void lmb_run_gate_leave(LmbRunGate *gate) {
    if (!gate) return;
    pthread_mutex_lock(&gate->lock);
    if (gate->in_use) gate->in_use--;
    admit(gate);
    pthread_mutex_unlock(&gate->lock);
}

uint32_t lmb_run_gate_inflight(const LmbRunGate *gate) {
    return gate ? atomic_load(&gate->published_in_use) : 0;
}

uint32_t lmb_run_gate_queued(const LmbRunGate *gate) {
    return gate ? atomic_load(&gate->published_queued) : 0;
}
