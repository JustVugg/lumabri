#define _POSIX_C_SOURCE 200809L
#include "lumabri_run_gate.h"

#include <errno.h>
#include <string.h>
#include <time.h>

struct LmbRunWaiter {
    struct LmbRunWaiter *next;
    int admitted;
};

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
        gate->in_use++;
        waiter->admitted = 1;
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
    if (pthread_cond_init(&gate->changed, NULL)) {
        pthread_mutex_destroy(&gate->lock); return -1;
    }
    publish(gate);
    return 0;
}

void lmb_run_gate_destroy(LmbRunGate *gate) {
    if (!gate) return;
    pthread_cond_destroy(&gate->changed);
    pthread_mutex_destroy(&gate->lock);
}

static void deadline_after(struct timespec *deadline, uint32_t wait_ms) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += wait_ms / 1000u;
    deadline->tv_nsec += (long)(wait_ms % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
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
    pthread_mutex_lock(&gate->lock);
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
    LmbRunWaiter waiter = {0};
    if (gate->tail) gate->tail->next = &waiter;
    else gate->head = &waiter;
    gate->tail = &waiter;
    gate->queued++;
    publish(gate);

    struct timespec deadline;
    deadline_after(&deadline, wait_ms);
    int result = 0;
    while (!waiter.admitted) {
        if (cancel && cancel(cancel_opaque)) { result = -1; break; }
        struct timespec poll_deadline;
        deadline_after(&poll_deadline, wait_ms < 100u ? wait_ms : 100u);
        if (poll_deadline.tv_sec > deadline.tv_sec ||
            (poll_deadline.tv_sec == deadline.tv_sec &&
             poll_deadline.tv_nsec > deadline.tv_nsec))
            poll_deadline = deadline;
        int rc = pthread_cond_timedwait(&gate->changed, &gate->lock,
                                        &poll_deadline);
        if (waiter.admitted) break;
        if (rc == ETIMEDOUT && poll_deadline.tv_sec == deadline.tv_sec &&
            poll_deadline.tv_nsec == deadline.tv_nsec) break;
    }
    if (waiter.admitted) result = 1;
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
