#ifndef LUMABRI_RUN_GATE_H
#define LUMABRI_RUN_GATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

typedef int (*LmbRunCancelFn)(void *opaque);

typedef struct LmbRunWaiter LmbRunWaiter;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    LmbRunWaiter *head;
    LmbRunWaiter *tail;
    uint32_t capacity;
    uint32_t max_queue;
    uint32_t in_use;
    uint32_t queued;
    _Atomic uint32_t published_in_use;
    _Atomic uint32_t published_queued;
} LmbRunGate;

int lmb_run_gate_init(LmbRunGate *gate, uint32_t capacity,
                      uint32_t max_queue);
void lmb_run_gate_destroy(LmbRunGate *gate);

/* FIFO admission. Returns 1 when admitted, 0 on queue limit/timeout and -1
 * when cancellation was requested. wait_ms is a real interactive deadline,
 * not the transport's long safety timeout. */
int lmb_run_gate_enter(LmbRunGate *gate, uint32_t wait_ms,
                       LmbRunCancelFn cancel, void *cancel_opaque);
void lmb_run_gate_leave(LmbRunGate *gate);

uint32_t lmb_run_gate_inflight(const LmbRunGate *gate);
uint32_t lmb_run_gate_queued(const LmbRunGate *gate);

#endif
