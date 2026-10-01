#ifndef LUMABRI_PREPARE_LIMITS_H
#define LUMABRI_PREPARE_LIMITS_H
#include <stdint.h>
/* Per engine, transient verified input. Released at seal, never a disk cache.
 * Four lanes cover interleaved gate/up/down reads; large protocol blocks
 * reduce the lane count so the same byte cap always holds. */
#define LMB_PREPARE_CACHE_BYTES (UINT64_C(64) << 20)
#define LMB_PREPARE_CACHE_SLOTS 4u
#endif
