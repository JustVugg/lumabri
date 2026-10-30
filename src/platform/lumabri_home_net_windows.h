/* Native household addressing and reserved listener handoff. SOCKET values
 * never pass through CRT descriptors or DuplicateHandle. */
#ifndef LMB_HOME_NET_WINDOWS_H
#define LMB_HOME_NET_WINDOWS_H
#include <iphlpapi.h>
#include <ipifcons.h>
#include "src/runtime/lumabri_pipe_windows.h"

static inline IP_ADAPTER_ADDRESSES *lmb_home_adapters(void) {
    ULONG bytes=16384;
    for (unsigned attempt=0;attempt<3;attempt++) {
        if (!bytes || bytes>1024u*1024u) return NULL;
        IP_ADAPTER_ADDRESSES *all=malloc(bytes);
        if (!all) return NULL;
        ULONG status=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|
            GAA_FLAG_SKIP_DNS_SERVER,NULL,all,&bytes);
        if (status==NO_ERROR) return all;
        free(all);
        if (status!=ERROR_BUFFER_OVERFLOW) return NULL;
    }
    return NULL;
}
static inline int lmb_home_interface_ip(char *out,size_t cap) {
    if (lmb_socket_init()) return -1;
    IP_ADAPTER_ADDRESSES *all=lmb_home_adapters(); int best=0;
    for (IP_ADAPTER_ADDRESSES *p=all;p;p=p->Next) {
        if (p->OperStatus!=IfOperStatusUp || p->IfType==IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u=p->FirstUnicastAddress;u;u=u->Next) {
            if (!u->Address.lpSockaddr || u->Address.iSockaddrLength<(int)sizeof(struct sockaddr_in) ||
                u->Address.lpSockaddr->sa_family!=AF_INET || u->DadState!=IpDadStatePreferred) continue;
            struct in_addr address=((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr;
            uint32_t host=ntohl(address.s_addr);
            if (!(host>>24) || (host>>24)==127 || (host>>16)==0xa9fe || (host>>28)>=14) continue;
            int private_ip=(host>>24)==10 || (host>>20)==0xac1 || (host>>16)==0xc0a8;
            int score=1+2*private_ip+(p->IfType!=IF_TYPE_PPP && p->IfType!=IF_TYPE_TUNNEL);
            if (score>best && inet_ntop(AF_INET,&address,out,cap)) best=score;
        }
    }
    free(all); return best ? 0 : -1;
}
static inline int lmb_home_advertise_ip(char *out,size_t cap) {
    const char *forced=getenv("LUMABRI_ADVERTISE");
    if (!forced || !*forced) return lmb_home_interface_ip(out,cap);
    struct in_addr address;
    if (inet_pton(AF_INET,forced,&address)!=1) { errno=EINVAL; return -1; }
    uint32_t host=ntohl(address.s_addr);
    if (!(host>>24) || (host>>28)>=14 || !inet_ntop(AF_INET,&address,out,cap)) { errno=EINVAL; return -1; }
    return 0;
}
static inline int lmb_home_subnet(const char *ip,char *out,size_t cap) {
    struct in_addr wanted;
    if (inet_pton(AF_INET,ip,&wanted)!=1) return -1;
    IP_ADAPTER_ADDRESSES *all=lmb_home_adapters(); int rc=-1;
    for (IP_ADAPTER_ADDRESSES *p=all;p;p=p->Next) {
        if (p->OperStatus!=IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u=p->FirstUnicastAddress;u;u=u->Next) {
            if (!u->Address.lpSockaddr || u->Address.iSockaddrLength<(int)sizeof(struct sockaddr_in) ||
                u->Address.lpSockaddr->sa_family!=AF_INET || u->OnLinkPrefixLength<8 || u->OnLinkPrefixLength>30 ||
                ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr!=wanted.s_addr) continue;
            uint32_t mask=UINT32_MAX<<(32-u->OnLinkPrefixLength);
            struct in_addr network={0}; network.s_addr=htonl(ntohl(wanted.s_addr)&mask);
            char address[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET,&network,address,sizeof address)) {
                int n=snprintf(out,cap,"%s/%u",address,(unsigned)u->OnLinkPrefixLength);
                rc=n>=0 && (size_t)n<cap ? 0 : -1;
            }
        }
    }
    free(all); return rc;
}
static inline LmbSocket lmb_home_listen_service(int *port) {
    int base=lmb_home_port_base();
    if (base<0) return LMB_INVALID_SOCKET;
    for (int n=base+1;n<base+LMB_HOME_PORT_COUNT;n++) {
        LmbSocket socket=lmb_listen(n);
        if (socket==LMB_INVALID_SOCKET) {
            if (errno==EADDRINUSE) continue;
            return LMB_INVALID_SOCKET;
        }
        *port=n; return socket; /* lmb_listen already disables inheritance */
    }
    errno=EADDRINUSE; return LMB_INVALID_SOCKET;
}

/* The owner retains both the listener and the private handoff channel until
 * the child's READY event. A failed startup is terminated through its owned
 * process job before the reservation is released. No ephemeral port rebind. */
static inline int lmb_home_export_listener(LmbSocket listener,DWORD child,HANDLE channel) {
    WSAPROTOCOL_INFOW protocol;
    if (!child) { errno=EINVAL; return -1; }
    if (WSADuplicateSocketW(listener,child,&protocol)) return lmb_socket_error();
    uint8_t header[8]={'L','M','B','L',1,0,0,0};
    uint64_t deadline=GetTickCount64()+2000;
    return lmb_pipe_io(channel,header,sizeof header,1,deadline) ||
           lmb_pipe_io(channel,&protocol,sizeof protocol,1,deadline) ? -1 : 0;
}
static inline LmbSocket lmb_home_take_listener(int port) {
    if (getenv("LUMABRI_HOME_LISTEN_FD")) { errno=EINVAL; return LMB_INVALID_SOCKET; }
    HANDLE channel;
    if (lmb_pipe_inherit(L"LUMABRI_HOME_LISTEN_HANDLE",&channel)) { errno=EINVAL; return LMB_INVALID_SOCKET; }
    if (channel==INVALID_HANDLE_VALUE) return lmb_listen(port);
    uint8_t header[8]; WSAPROTOCOL_INFOW protocol;
    uint64_t deadline=GetTickCount64()+2000;
    int bad=lmb_pipe_io(channel,header,sizeof header,0,deadline) || memcmp(header,"LMBL\1\0\0",8) ||
        lmb_pipe_io(channel,&protocol,sizeof protocol,0,deadline);
    CloseHandle(channel);
    if (bad || protocol.iAddressFamily!=AF_INET || protocol.iSocketType!=SOCK_STREAM ||
        protocol.iProtocol!=IPPROTO_TCP || lmb_socket_init()) { errno=EINVAL; return LMB_INVALID_SOCKET; }
    LmbSocket socket=WSASocketW(FROM_PROTOCOL_INFO,FROM_PROTOCOL_INFO,FROM_PROTOCOL_INFO,&protocol,0,
        WSA_FLAG_OVERLAPPED|WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket==LMB_INVALID_SOCKET) { (void)lmb_socket_error(); return socket; }
    struct sockaddr_in address={0}; int length=sizeof address,type=0,size=sizeof type;
    if (getsockname(socket,(struct sockaddr *)&address,&length) || length!=sizeof address ||
        address.sin_family!=AF_INET || ntohs(address.sin_port)!=port ||
        getsockopt(socket,SOL_SOCKET,SO_TYPE,(char *)&type,&size) || type!=SOCK_STREAM ||
        listen(socket,64) || lmb_socket_noinherit(socket)) {
        lmb_socket_close(socket); errno=EINVAL; return LMB_INVALID_SOCKET;
    }
    return socket;
}
#endif
