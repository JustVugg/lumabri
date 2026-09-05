/* Child-to-supervisor readiness: never infer readiness from a port probe. */
#ifndef LUMABRI_READY_H
#define LUMABRI_READY_H
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
static int lmb_ready_notify(void) {
    const char *s = getenv("LUMABRI_READY_FD");
    if (!s || !*s) return 0;
    char *end;
    long fd = strtol(s, &end, 10);
    if (*end || fd < 3 || fd > INT_MAX) return -1;
    int rc = write((int)fd, "R", 1) == 1 ? 0 : -1;
    close((int)fd);
    unsetenv("LUMABRI_READY_FD");
    return rc;
}
#endif
