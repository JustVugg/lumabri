/* Nonblocking wakeups for the tracker control loop. A full pipe already
 * contains a wakeup; request state itself lives behind the peer mutex. */
#ifndef LUMABRI_WAKEUP_H
#define LUMABRI_WAKEUP_H
#include "lumabri_proto.h"
#ifdef _WIN32
typedef LmbSocket LmbWakeup;
#define LMB_INVALID_WAKEUP LMB_INVALID_SOCKET
static inline LmbWakeup lmb_wakeup_open(LmbWakeup *writer) {
    LmbSocket listener=LMB_INVALID_SOCKET,reader=LMB_INVALID_SOCKET;
    *writer=LMB_INVALID_SOCKET;
    struct sockaddr_in address={0},local={0},peer={0};
    LmbSocketLength size=sizeof address;
    address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    listener=lmb_socket_open(AF_INET,SOCK_STREAM,0);
    if (listener==LMB_INVALID_SOCKET || bind(listener,(struct sockaddr *)&address,sizeof address) ||
        listen(listener,1) || getsockname(listener,(struct sockaddr *)&address,&size)) goto fail;
    *writer=lmb_socket_open(AF_INET,SOCK_STREAM,0);
    if (*writer==LMB_INVALID_SOCKET || lmb_socket_connect(*writer,(struct sockaddr *)&address,sizeof address)) goto fail;
    size=sizeof peer;reader=lmb_socket_accept(listener,(struct sockaddr *)&peer,&size);
    size=sizeof local;
    if (reader==LMB_INVALID_SOCKET || getsockname(*writer,(struct sockaddr *)&local,&size) ||
        peer.sin_addr.s_addr!=local.sin_addr.s_addr || peer.sin_port!=local.sin_port ||
        lmb_socket_nonblocking(reader,1) || lmb_socket_nonblocking(*writer,1)) goto fail;
    int one=1;
    if (setsockopt(*writer,IPPROTO_TCP,TCP_NODELAY,(const char *)&one,sizeof one)) goto fail;
    lmb_socket_close(listener);return reader;
fail:
    if (listener!=LMB_INVALID_SOCKET) lmb_socket_close(listener);
    if (reader!=LMB_INVALID_SOCKET) lmb_socket_close(reader);
    if (*writer!=LMB_INVALID_SOCKET) lmb_socket_close(*writer);
    *writer=LMB_INVALID_SOCKET;return LMB_INVALID_SOCKET;
}
static inline void lmb_wakeup_close(LmbWakeup wake) {lmb_socket_close(wake);}
static inline void lmb_wakeup_notify(LmbWakeup wake) {char one=1;(void)send(wake,&one,1,0);}
static inline void lmb_wakeup_drain(LmbWakeup wake) {char bytes[64];while (recv(wake,bytes,sizeof bytes,0)>0) {}}
static inline int lmb_wakeup_wait(LmbSocket socket,LmbWakeup wake,int *notified) {
    WSAPOLLFD fds[2]={{socket,POLLIN,0},{wake,POLLIN,0}};
    int rc=WSAPoll(fds,2,-1);if (rc==SOCKET_ERROR) return lmb_socket_error();
    *notified=!!(fds[1].revents&POLLIN);return !!(fds[0].revents&(POLLIN|POLLERR|POLLHUP|POLLNVAL));
}
#else
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
typedef int LmbWakeup;
#define LMB_INVALID_WAKEUP (-1)
static inline LmbWakeup lmb_wakeup_open(LmbWakeup *writer) {
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
static inline void lmb_wakeup_close(LmbWakeup wake) {close(wake);}
static inline void lmb_wakeup_notify(LmbWakeup wake) {uint64_t one=1;ssize_t sent=write(wake,&one,sizeof one);(void)sent;}
static inline void lmb_wakeup_drain(LmbWakeup wake) {uint64_t value;while (read(wake,&value,sizeof value)==sizeof value) {}}
static inline int lmb_wakeup_wait(LmbSocket socket,LmbWakeup wake,int *notified) {
    struct pollfd fds[2]={{socket,POLLIN,0},{wake,POLLIN,0}};
    if (poll(fds,2,-1)<0) return -1;
    *notified=!!(fds[1].revents&POLLIN);return !!(fds[0].revents&(POLLIN|POLLERR|POLLHUP|POLLNVAL));
}
#endif
#endif
