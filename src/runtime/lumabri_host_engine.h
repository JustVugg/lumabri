/* Borrowed engine pipes and negotiated capabilities. Process ownership stays
 * with the launcher; this view is shared by both native hosted gateways. */
#ifndef LMB_HOST_ENGINE_H
#define LMB_HOST_ENGINE_H
#include "src/runtime/lumabri_request_seed.h"
#include "src/protocol/lumabri_socket.h"
#ifdef _WIN32
#include "src/runtime/lumabri_pipe_windows.h"
typedef HANDLE LmbHostPipe;
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
typedef int LmbHostPipe;
#endif
typedef struct {
    LmbHostPipe to,from;
    int segment,reset_supported;
    uint32_t session_slots;
    int greedy_only,request_seed_supported,vision_supported;
    uint32_t numeric_abi;
    char numeric_class[97];
} LmbHostEngine;

static inline void host_socket_shutdown(LmbSocket socket) {
#ifdef _WIN32
    (void)shutdown(socket,SD_BOTH);
#else
    (void)shutdown(socket,SHUT_RDWR);
#endif
}

static inline int host_engine_prepare(LmbHostEngine *e) {
#ifdef _WIN32
    return GetFileType(e->to)==FILE_TYPE_PIPE && GetFileType(e->from)==FILE_TYPE_PIPE ? 0 : -1;
#else
    int flags=fcntl(e->to,F_GETFL,0);
    return flags<0 || fcntl(e->to,F_SETFL,flags|O_NONBLOCK) ? -1 : 0;
#endif
}

/* Only the turn-gate owner writes. A stalled Edge cannot retain that gate
 * indefinitely. Native handles are overlapped, private and explicitly owned. */
static inline int host_engine_write(LmbHostEngine *e,const void *data,size_t bytes) {
#ifdef _WIN32
    if (g_stopping) return -1;
    return lmb_pipe_io(e->to,(void *)data,bytes,1,GetTickCount64()+5000);
#else
    const uint8_t *p=data;double deadline=nowd()+5;
    while (bytes) {
        if (g_stopping || nowd()>=deadline) {errno=ETIMEDOUT;return -1;}
        ssize_t n=write(e->to,p,bytes);
        if (n>0) {p+=n;bytes-=(size_t)n;continue;}
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            struct pollfd fd={e->to,POLLOUT,0};
            if (poll(&fd,1,100)<0 && errno!=EINTR) return -1;
            continue;
        }
        return -1;
    }
    return 0;
#endif
}

enum {HOST_SOCKET_READ=1,HOST_ENGINE_READ=2,HOST_SOCKET_CLOSED=4,HOST_ENGINE_CLOSED=8};
static inline int host_engine_wait(LmbSocket socket,LmbHostEngine *e,int timeout_ms) {
#ifdef _WIN32
    uint64_t deadline=GetTickCount64()+(unsigned)timeout_ms;
    for (;;) {
        int ready=0;
        if (socket!=LMB_INVALID_SOCKET) {
            int n=lmb_socket_wait(socket,0,0);
            if (n) ready|=n>0 ? HOST_SOCKET_READ : HOST_SOCKET_CLOSED;
        }
        DWORD bytes=0;
        if (!PeekNamedPipe(e->from,NULL,0,NULL,&bytes,NULL)) ready|=HOST_ENGINE_CLOSED;
        else if (bytes) ready|=HOST_ENGINE_READ;
        uint64_t now=GetTickCount64();
        if (ready || g_stopping || now>=deadline) return ready;
        Sleep((DWORD)(deadline-now<2 ? deadline-now : 2));
    }
#else
    struct pollfd p[2]={{socket,POLLIN,0},{e->from,POLLIN,0}};
    int n=poll(p,2,timeout_ms);
    if (n<0) return errno==EINTR ? 0 : -1;
    return ((p[0].revents&POLLIN) ? HOST_SOCKET_READ : 0) |
        ((p[1].revents&POLLIN) ? HOST_ENGINE_READ : 0) |
        ((p[0].revents&(POLLERR|POLLHUP|POLLNVAL)) ? HOST_SOCKET_CLOSED : 0) |
        ((p[1].revents&(POLLERR|POLLHUP|POLLNVAL)) ? HOST_ENGINE_CLOSED : 0);
#endif
}

/* Called only after readiness. No second consumer may read these pipes. */
static inline int host_engine_read(LmbHostEngine *e,void *out,size_t capacity) {
#ifdef _WIN32
    DWORD available=0;
    if (!PeekNamedPipe(e->from,NULL,0,NULL,&available,NULL) || !available) return -1;
    size_t count=available<capacity ? available : capacity;
    if (count>INT_MAX || lmb_pipe_io(e->from,out,count,0,GetTickCount64()+5000)) return -1;
    return (int)count;
#else
    ssize_t n;
    do {n=read(e->from,out,capacity);} while (n<0 && errno==EINTR);
    return n>INT_MAX ? -1 : (int)n;
#endif
}
#endif
