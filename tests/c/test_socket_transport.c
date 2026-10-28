#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "lumabri_proto.h"
#include <assert.h>
#include <pthread.h>
#ifdef _WIN32
#include <fcntl.h>
#endif

#ifdef _WIN32
_Static_assert(sizeof(LmbSocket)==sizeof(SOCKET),"Socket handles must not be truncated");
#endif

static void pair(LmbSocket *client,LmbSocket *server) {
    LmbSocket listener=lmb_listen(0); assert(listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in addr; LmbSocketLength length=sizeof addr;
    assert(!getsockname(listener,(struct sockaddr *)&addr,&length));
    assert(lmb_listen(ntohs(addr.sin_port))==LMB_INVALID_SOCKET);
    char endpoint[64]; snprintf(endpoint,sizeof endpoint,"127.0.0.1:%u",ntohs(addr.sin_port));
    *client=lmb_connect_ms_io(endpoint,1000,2000); assert(*client!=LMB_INVALID_SOCKET);
    *server=lmb_socket_accept(listener,NULL,NULL); assert(*server!=LMB_INVALID_SOCKET);
    lmb_set_io_timeout(*server,2000);
    assert(!lmb_socket_nodelay(*server));
#ifdef _WIN32
    DWORD flags=~0u;
    assert(GetHandleInformation((HANDLE)*client,&flags) && !(flags&HANDLE_FLAG_INHERIT));
    assert(GetHandleInformation((HANDLE)*server,&flags) && !(flags&HANDLE_FLAG_INHERIT));
    DWORD timeout=0; int size=sizeof timeout;
    assert(!getsockopt(*server,SOL_SOCKET,SO_RCVTIMEO,(char *)&timeout,&size));
    assert(timeout==2000);
#else
    assert(fcntl(*client,F_GETFD)&FD_CLOEXEC);
    assert(fcntl(*server,F_GETFD)&FD_CLOEXEC);
#endif
    lmb_close(listener);
}

static void *echo(void *opaque) {
    LmbSocket fd=*(LmbSocket *)opaque;
    for (unsigned i=0;i<8;i++) {
        LmbMsg m={0}; assert(!lmb_recv_bounded(fd,&m,128*1024,2000));
        assert(m.op==LMB_SEG_RUN && m.body_len==4 && m.pay_len==65536);
        assert(lmb_get32(m.body)==i);
        for (uint32_t j=0;j<m.pay_len;j++) assert(m.pay[j]==(unsigned char)(i+j));
        assert(!lmb_send(fd,LMB_SEG_RUN_R,m.body,m.body_len,m.pay,m.pay_len));
        lmb_msg_free(&m);
    }
    return NULL;
}

static FILE *metadata_file(void) {
#ifdef _WIN32
    /* The MinGW tmpfile implementation may target the drive root. Use the
     * caller's actual temporary directory, with kernel delete-on-close. */
    char directory[MAX_PATH],path[MAX_PATH];
    DWORD length=GetTempPathA(sizeof directory,directory);
    assert(length && length<sizeof directory);
    assert(GetTempFileNameA(directory,"lmb",0,path));
    HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,
                           FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    assert(file!=INVALID_HANDLE_VALUE);
    int fd=_open_osfhandle((intptr_t)file,_O_RDWR|_O_BINARY); assert(fd>=0);
    FILE *stream=_fdopen(fd,"w+b"); assert(stream); return stream;
#else
    return tmpfile();
#endif
}

int main(void) {
    LmbSocket c,s; pair(&c,&s);
    pthread_t thread; assert(!pthread_create(&thread,NULL,echo,&s));
    for (unsigned i=0;i<8;i++) {
        unsigned char body[4],pay[65536]; lmb_put32(body,i);
        for (size_t j=0;j<sizeof pay;j++) pay[j]=(unsigned char)(i+j);
        assert(!lmb_send(c,LMB_SEG_RUN,body,sizeof body,pay,sizeof pay));
        LmbMsg m={0}; assert(!lmb_recv_bounded(c,&m,128*1024,2000));
        assert(m.op==LMB_SEG_RUN_R && m.body_len==sizeof body && m.pay_len==sizeof pay);
        assert(!memcmp(m.body,body,sizeof body) && !memcmp(m.pay,pay,sizeof pay));
        lmb_msg_free(&m);
    }
    assert(!pthread_join(thread,NULL));
    assert(!atomic_load(&lmb_rx_inflight));

    /* The metadata reader must still use CRT/POSIX file I/O while sockets live. */
    FILE *file=metadata_file(); assert(file);
    assert(fwrite("metadata",1,8,file)==8 && !fflush(file)); rewind(file);
    char text[8]; assert(!lmb_read_full(fileno(file),text,sizeof text));
    assert(!memcmp(text,"metadata",8)); fclose(file);
    lmb_close(c); LmbMsg m={0};
    assert(lmb_recv_bounded(s,&m,1024,1000)<0 && errno==ECONNRESET); lmb_close(s);

    /* One deadline for an incomplete header, not a new timeout per byte. */
    pair(&c,&s); unsigned char header[16]={0};
    lmb_put32(header,LMB_MAGIC); lmb_put32(header+4,LMB_PING);
    assert(!lmb_write_full(c,header,8));
    uint64_t start=lmb_socket_now_ms();
    assert(lmb_recv_bounded(s,&m,1024,100)<0 && errno==ETIMEDOUT);
    assert(lmb_socket_now_ms()-start<1500 && !lmb_read_deadline_ms);
    assert(!atomic_load(&lmb_rx_inflight)); lmb_close(c); lmb_close(s);

    /* Reject bad wire lengths before allocation and expose connection refusal. */
    pair(&c,&s); lmb_put32(header+8,LMB_MAX_BODY+1u);
    assert(!lmb_write_full(c,header,sizeof header));
    assert(lmb_recv_bounded(s,&m,1024,1000)<0 && errno==EMSGSIZE);
    assert(!atomic_load(&lmb_rx_inflight)); lmb_close(c); lmb_close(s);
    LmbSocket reserved=lmb_socket_open(AF_INET,SOCK_STREAM,0); assert(reserved!=LMB_INVALID_SOCKET);
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(reserved,(struct sockaddr *)&addr,sizeof addr));
    LmbSocketLength size=sizeof addr; assert(!getsockname(reserved,(struct sockaddr *)&addr,&size));
    char endpoint[64]; snprintf(endpoint,sizeof endpoint,"127.0.0.1:%u",ntohs(addr.sin_port));
    start=lmb_socket_now_ms();
    assert(lmb_connect_ms(endpoint,2000)==LMB_INVALID_SOCKET);
    fprintf(stderr,"Closed listener: error=%d (%s), elapsed=%llu ms\n",lmb_connect_errno,
            strerror(lmb_connect_errno),(unsigned long long)(lmb_socket_now_ms()-start));
    /* Windows may retry a refused loopback SYN until the caller's deadline.
     * Both outcomes must retain their meaning, with no unbounded wait. */
    assert((lmb_connect_errno==ECONNREFUSED || lmb_connect_errno==ETIMEDOUT) &&
           lmb_socket_now_ms()-start<3000);
    lmb_close(reserved);
    puts("NATIVE SOCKET TRANSPORT: PASS (wire frames, handle ownership, deadlines, EOF, bounds, file isolation)");
    return 0;
}
