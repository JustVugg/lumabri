/* Native socket operations. Handles never pass through a CRT file descriptor.
 * POSIX callers retain their existing int ABI; Windows SOCKET is pointer-sized. */
#ifndef LUMABRI_SOCKET_H
#define LUMABRI_SOCKET_H
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
typedef SOCKET LmbSocket;
typedef int LmbSocketLength;
#define LMB_INVALID_SOCKET INVALID_SOCKET
static inline int lmb_socket_map_error(int error) {
    switch (error) {
    case 0: return 0;
    case WSAEINTR: return EINTR;
    case WSAEACCES: return EACCES;
    case WSAEFAULT: return EFAULT;
    case WSAEINVAL: return EINVAL;
    case WSAEMFILE: return EMFILE;
    case WSAEWOULDBLOCK: return EWOULDBLOCK;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENETDOWN: return ENETDOWN;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEISCONN: return EISCONN;
    case WSAENOTCONN: return ENOTCONN;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    default: return EIO;
    }
}
static inline int lmb_socket_error(void) {
    errno=lmb_socket_map_error(WSAGetLastError()); return -1;
}
static INIT_ONCE lmb_winsock_once=INIT_ONCE_STATIC_INIT;
static int lmb_winsock_status;
static BOOL CALLBACK lmb_winsock_start(PINIT_ONCE once,PVOID arg,PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    WSADATA data;
    lmb_winsock_status=WSAStartup(MAKEWORD(2,2),&data);
    if (!lmb_winsock_status && data.wVersion!=MAKEWORD(2,2)) {
        WSACleanup(); lmb_winsock_status=WSAVERNOTSUPPORTED;
    }
    return TRUE;
}
/* Process lifetime initialization: no cleanup can invalidate another thread's
 * live sockets. Windows releases the subsystem on process exit. */
static inline int lmb_socket_init(void) {
    if (!InitOnceExecuteOnce(&lmb_winsock_once,lmb_winsock_start,NULL,NULL)) {
        errno=EIO; return -1;
    }
    if (lmb_winsock_status) { errno=EIO; return -1; }
    return 0;
}
static inline int lmb_socket_close(LmbSocket fd) {
    return closesocket(fd)==SOCKET_ERROR ? lmb_socket_error() : 0;
}
static inline int lmb_socket_noinherit(LmbSocket fd) {
    if (SetHandleInformation((HANDLE)fd,HANDLE_FLAG_INHERIT,0)) return 0;
    errno=EIO; return -1;
}
static inline int lmb_socket_nonblocking(LmbSocket fd,int on) {
    u_long mode=on!=0;
    return ioctlsocket(fd,FIONBIO,&mode)==SOCKET_ERROR ? lmb_socket_error() : 0;
}
static inline uint64_t lmb_socket_now_ms(void) { return GetTickCount64(); }
static inline ptrdiff_t lmb_socket_send(LmbSocket fd,const void *data,size_t size) {
    int n=send(fd,(const char *)data,size>INT_MAX ? INT_MAX : (int)size,0);
    return n==SOCKET_ERROR ? lmb_socket_error() : n;
}
static inline ptrdiff_t lmb_socket_receive(LmbSocket fd,void *data,size_t size) {
    int n=recv(fd,(char *)data,size>INT_MAX ? INT_MAX : (int)size,0);
    return n==SOCKET_ERROR ? lmb_socket_error() : n;
}
static inline int lmb_socket_wait(LmbSocket fd,int writing,int timeout_ms) {
    fd_set ready,errors; FD_ZERO(&ready); FD_ZERO(&errors);
    FD_SET(fd,&ready); FD_SET(fd,&errors);
    struct timeval timeout={timeout_ms/1000,(timeout_ms%1000)*1000};
    int n=select(0,writing ? NULL : &ready,writing ? &ready : NULL,&errors,&timeout);
    return n==SOCKET_ERROR ? lmb_socket_error() : n ? 1 : 0;
}
static inline int lmb_socket_option(LmbSocket fd,int level,int name,
                                     const void *value,LmbSocketLength size) {
    return setsockopt(fd,level,name,(const char *)value,size)==SOCKET_ERROR ? lmb_socket_error() : 0;
}
static inline int lmb_socket_timeout(LmbSocket fd,int ms) {
    DWORD value=(DWORD)ms;
    return lmb_socket_option(fd,SOL_SOCKET,SO_RCVTIMEO,&value,sizeof value) ||
           lmb_socket_option(fd,SOL_SOCKET,SO_SNDTIMEO,&value,sizeof value) ? -1 : 0;
}
static inline int lmb_socket_peer_error(LmbSocket fd) {
    int value=0,length=sizeof value;
    if (getsockopt(fd,SOL_SOCKET,SO_ERROR,(char *)&value,&length)) return lmb_socket_error();
    if (value) { errno=lmb_socket_map_error(value); return -1; }
    return 0;
}
static inline int lmb_socket_bind_policy(LmbSocket fd) {
    int one=1; /* SO_REUSEADDR on Windows allows another socket to steal the port. */
    return lmb_socket_option(fd,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,&one,sizeof one);
}
#else
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int LmbSocket;
typedef socklen_t LmbSocketLength;
#define LMB_INVALID_SOCKET (-1)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static inline int lmb_socket_init(void) { return 0; }
static inline int lmb_socket_error(void) { return -1; }
static inline int lmb_socket_close(LmbSocket fd) { return close(fd); }
static inline int lmb_socket_noinherit(LmbSocket fd) {
    int flags=fcntl(fd,F_GETFD); return flags<0 ? -1 : fcntl(fd,F_SETFD,flags|FD_CLOEXEC);
}
static inline int lmb_socket_nonblocking(LmbSocket fd,int on) {
    int flags=fcntl(fd,F_GETFL);
    return flags<0 ? -1 : fcntl(fd,F_SETFL,on ? flags|O_NONBLOCK : flags&~O_NONBLOCK);
}
static inline uint64_t lmb_socket_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)) return 0;
    return (uint64_t)now.tv_sec*1000u+(uint64_t)now.tv_nsec/1000000u;
}
static inline ptrdiff_t lmb_socket_send(LmbSocket fd,const void *data,size_t size) {
    return send(fd,data,size,MSG_NOSIGNAL);
}
static inline ptrdiff_t lmb_socket_receive(LmbSocket fd,void *data,size_t size) {
    return recv(fd,data,size,0);
}
static inline int lmb_socket_wait(LmbSocket fd,int writing,int timeout_ms) {
    struct pollfd p={fd,writing ? POLLOUT : POLLIN,0};
    int n=poll(&p,1,timeout_ms);
    if (n>0 && (p.revents&POLLNVAL)) { errno=EBADF; return -1; }
    return n;
}
static inline int lmb_socket_option(LmbSocket fd,int level,int name,
                                     const void *value,LmbSocketLength size) {
    return setsockopt(fd,level,name,value,size);
}
static inline int lmb_socket_timeout(LmbSocket fd,int ms) {
    struct timeval value={ms/1000,(ms%1000)*1000};
    return lmb_socket_option(fd,SOL_SOCKET,SO_RCVTIMEO,&value,sizeof value) ||
           lmb_socket_option(fd,SOL_SOCKET,SO_SNDTIMEO,&value,sizeof value) ? -1 : 0;
}
static inline int lmb_socket_peer_error(LmbSocket fd) {
    int value=0; socklen_t length=sizeof value;
    if (getsockopt(fd,SOL_SOCKET,SO_ERROR,&value,&length)) return -1;
    if (value) { errno=value; return -1; }
    return 0;
}
static inline int lmb_socket_bind_policy(LmbSocket fd) {
    int one=1; return lmb_socket_option(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
}
#endif

static inline LmbSocket lmb_socket_open(int family,int type,int protocol) {
    if (lmb_socket_init()) return LMB_INVALID_SOCKET;
#ifdef _WIN32
    LmbSocket fd=WSASocketW(family,type,protocol,NULL,0,
                           WSA_FLAG_OVERLAPPED|WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    LmbSocket fd=socket(family,type|SOCK_CLOEXEC,protocol);
#else
    LmbSocket fd=socket(family,type,protocol);
#endif
    if (fd==LMB_INVALID_SOCKET) { (void)lmb_socket_error(); return fd; }
    if (lmb_socket_noinherit(fd)) {
        int saved=errno; lmb_socket_close(fd); errno=saved; return LMB_INVALID_SOCKET;
    }
    return fd;
}
static inline LmbSocket lmb_socket_accept(LmbSocket listener,struct sockaddr *addr,
                                          LmbSocketLength *length) {
    LmbSocket fd=accept(listener,addr,length);
    if (fd==LMB_INVALID_SOCKET) { (void)lmb_socket_error(); return fd; }
    if (lmb_socket_noinherit(fd)) {
        int saved=errno; lmb_socket_close(fd); errno=saved; return LMB_INVALID_SOCKET;
    }
    return fd;
}
static inline int lmb_socket_connect(LmbSocket fd,const struct sockaddr *addr,
                                      LmbSocketLength length) {
    return connect(fd,addr,length) ? lmb_socket_error() : 0;
}
static inline int lmb_socket_nodelay(LmbSocket fd) {
    int one=1; return lmb_socket_option(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
}
#endif
