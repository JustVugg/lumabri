/* Nonblocking wakeups for the tracker control loop. A full pipe already
 * contains a wakeup; request state itself lives behind the peer mutex. */
#ifndef LUMABRI_WAKEUP_H
#define LUMABRI_WAKEUP_H
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static inline int lmb_wakeup_open(int *writer) {
    int pair[2];
    *writer = -1;
    if (pipe(pair)) return -1;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(pair[i], F_GETFL);
        if (flags < 0 || fcntl(pair[i], F_SETFL, flags | O_NONBLOCK) ||
            fcntl(pair[i], F_SETFD, FD_CLOEXEC)) {
            int saved = errno;
            close(pair[0]); close(pair[1]); errno = saved; return -1;
        }
    }
    *writer = pair[1];
    return pair[0];
}
#endif
