#define _DEFAULT_SOURCE
#include "lumabri_run_gate.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

static int cancelled(void *opaque);

typedef struct {
    LmbRunGate *gate;
    int id;
    uint32_t wait_ms;
    _Atomic int *order_index;
    int *order;
    int result;
    _Atomic int *stop;
} Worker;

static void *worker(void *opaque) {
    Worker *work = opaque;
    work->result = lmb_run_gate_enter(work->gate, work->wait_ms,
                                    work->stop ? cancelled : NULL, work->stop);
    if (work->result == 1) {
        int index = atomic_fetch_add(work->order_index, 1);
        work->order[index] = work->id;
        usleep(30000);
        lmb_run_gate_leave(work->gate);
    }
    return NULL;
}

static int cancelled(void *opaque) {
    return atomic_load((_Atomic int *)opaque);
}

static void wait_queued(LmbRunGate *gate, unsigned count) {
    for (unsigned i = 0; i < 5000 && lmb_run_gate_queued(gate) != count; i++) usleep(1000);
    assert(lmb_run_gate_queued(gate) == count);
}

int main(void) {
    LmbRunGate gate;
    assert(!lmb_run_gate_init(&gate, 1, 2));
    assert(lmb_run_gate_enter(&gate, 100, NULL, NULL) == 1);

    _Atomic int index = 0;
    int order[2] = {-1, -1};
    Worker works[3] = {
        { &gate, 1, 5000, &index, order, 0, NULL },
        { &gate, 2, 5000, &index, order, 0, NULL },
        { &gate, 3, 20, &index, order, 0, NULL },
    };
    pthread_t threads[3];
    pthread_create(&threads[0], NULL, worker, &works[0]);
    wait_queued(&gate, 1);
    pthread_create(&threads[1], NULL, worker, &works[1]);
    wait_queued(&gate, 2);
    pthread_create(&threads[2], NULL, worker, &works[2]);
    pthread_join(threads[2], NULL);
    assert(works[2].result == 0); /* bounded queue rejects immediately */
    lmb_run_gate_leave(&gate);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    assert(works[0].result == 1 && works[1].result == 1);
    assert(order[0] == 1 && order[1] == 2); /* FIFO, no chat starvation */
    assert(!lmb_run_gate_inflight(&gate) && !lmb_run_gate_queued(&gate));

    assert(lmb_run_gate_enter(&gate, 100, NULL, NULL) == 1);
    Worker timeout = { &gate, 4, 20, &index, order, 0, NULL };
    pthread_create(&threads[0], NULL, worker, &timeout);
    pthread_join(threads[0], NULL);
    assert(timeout.result == 0);
    _Atomic int stop = 1;
    assert(lmb_run_gate_enter(&gate, 100, cancelled, &stop) == -1);
    lmb_run_gate_leave(&gate);
    /* Free capacity is not authority to run an already-cancelled request. */
    assert(lmb_run_gate_enter(&gate, 100, cancelled, &stop) == -1);
    assert(!lmb_run_gate_inflight(&gate));
    atomic_store(&stop, 0);
    assert(lmb_run_gate_enter(&gate, 100, NULL, NULL) == 1);
    Worker abandoning = {&gate, 5, 5000, &index, order, 0, &stop};
    pthread_create(&threads[0], NULL, worker, &abandoning);
    wait_queued(&gate, 1);
    atomic_store(&stop, 1);
    lmb_run_gate_leave(&gate);
    pthread_join(threads[0], NULL);
    assert(abandoning.result == -1);
    assert(!lmb_run_gate_inflight(&gate) && !lmb_run_gate_queued(&gate));
    lmb_run_gate_destroy(&gate);
    puts("RUN GATE: PASS (FIFO, bounded queue, deadline, cancellation, telemetry)");
    return 0;
}
