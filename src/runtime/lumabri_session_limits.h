#ifndef LMB_SESSION_LIMITS_H
#define LMB_SESSION_LIMITS_H
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#define LMB_HOST_MAX_SESSIONS 8u
/* Buffered request, protocol metadata and host worker stack allowance. */
#define LMB_HOST_SESSION_BYTES (UINT64_C(12) << 20)

static inline int lmb_session_limit_parse(const char *text, uint32_t maximum, uint32_t *value) {
    if (!text || !*text || !value) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || !n || n > maximum) return -1;
    *value = (uint32_t)n; return 0;
}
#endif
