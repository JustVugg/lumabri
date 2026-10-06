/* Lumabri's optional seventh SUBMIT field. A request-scoped sampler seed
 * makes a fresh, compatible replica replay the same stochastic draw stream.
 * Byte-prefix verification is still required before resuming visible output. */
#ifndef LMB_REQUEST_SEED_H
#define LMB_REQUEST_SEED_H
#include <stdint.h>
#include <stddef.h>
static inline int lmb_request_seed(const char *text, uint64_t *seed) {
    if (!text || !*text || !seed) return -1;
    uint64_t value=0;
    for (size_t i=0;text[i];i++) {
        if (i>=20 || text[i]<'0' || text[i]>'9' || value>(UINT64_MAX-(unsigned)(text[i]-'0'))/10) return -1;
        value=value*10+(unsigned)(text[i]-'0');
    }
    *seed=value; return 0;
}
#define LMB_REPLICA_RETRYABLE "LMB_REPLICA_UNAVAILABLE "
#endif
