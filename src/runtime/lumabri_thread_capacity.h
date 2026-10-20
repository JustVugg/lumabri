#ifndef LUMABRI_THREAD_CAPACITY_H
#define LUMABRI_THREAD_CAPACITY_H

#ifdef _OPENMP
#include <omp.h>
#endif

/* Ask the installed engine, not the UI's compiler. Respect the operator's
 * OpenMP limits; this is startup information, not measured model capacity. */
static unsigned lmb_engine_thread_capacity(void) {
#ifdef _OPENMP
    int capacity = omp_get_num_procs();
    int configured = omp_get_max_threads();
    int limit = omp_get_thread_limit();
    if (configured > 0 && configured < capacity) capacity = configured;
    if (limit > 0 && limit < capacity) capacity = limit;
    if (capacity < 1) capacity = 1;
    if (capacity > 256) capacity = 256;
    return (unsigned)capacity;
#else
    return 1;
#endif
}
#endif
