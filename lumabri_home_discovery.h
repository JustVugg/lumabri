/* LAN-only discovery hints. No key/token/model data is broadcast. A result
 * is NOT trusted: the user compares its identity with the host, and the
 * encrypted connection must prove that identity before AUTH is sent. */
#ifndef LUMABRI_HOME_DISCOVERY_H
#define LUMABRI_HOME_DISCOVERY_H
#include "lumabri_home_net.h"
#include <pthread.h>

#define LMB_HOME_DISC_QUERY 24
#define LMB_HOME_DISC_REPLY 122
#define LMB_HOME_DISC_MAX 8
typedef struct { char address[64], name[64]; uint8_t identity[32]; } LmbHomeFound;

static inline uint64_t lmb_home_disc_ms(void) {
    return lmb_socket_now_ms();
}

static inline int lmb_home_on_link(struct in_addr peer) {
    if ((ntohl(peer.s_addr) >> 24) == 127) return 1;
#ifdef _WIN32
    IP_ADAPTER_ADDRESSES *all=lmb_home_adapters();int found=0;
    for (IP_ADAPTER_ADDRESSES *p=all;p;p=p->Next) {
        if (p->OperStatus!=IfOperStatusUp || p->IfType==IF_TYPE_SOFTWARE_LOOPBACK || p->IfType==IF_TYPE_PPP) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u=p->FirstUnicastAddress;u;u=u->Next) {
            if (!u->Address.lpSockaddr || u->Address.iSockaddrLength<(int)sizeof(struct sockaddr_in) ||
                u->Address.lpSockaddr->sa_family!=AF_INET || !u->OnLinkPrefixLength || u->OnLinkPrefixLength>32) continue;
            uint32_t mask=htonl(UINT32_MAX<<(32-u->OnLinkPrefixLength));
            uint32_t local=((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
            if ((local&mask)==(peer.s_addr&mask)) found=1;
        }
    }
    free(all);return found;
#else
    struct ifaddrs *all = NULL; int found = 0;
    if (getifaddrs(&all)) return 0;
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET ||
            !(p->ifa_flags & IFF_UP) || (p->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) continue;
        struct in_addr local = ((struct sockaddr_in *)p->ifa_addr)->sin_addr;
        uint32_t mask = ((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr;
        if (mask && (peer.s_addr & mask) == (local.s_addr & mask)) found = 1;
    }
    freeifaddrs(all); return found;
#endif
}

static inline int lmb_home_disc_parse(const uint8_t *p, size_t len,
    const uint8_t nonce[16], const struct sockaddr_in *from, LmbHomeFound *out) {
    if (len != LMB_HOME_DISC_REPLY || memcmp(p, "LMBHOME1", 8) ||
        memcmp(p + 8, nonce, 16) || !memchr(p + 58, 0, 64)) return -1;
    unsigned port = p[56] | ((unsigned)p[57] << 8), any = 0;
    if (port < 1024 || !p[58]) return -1;
    for (int i = 0; i < 32; i++) any |= p[24 + i];
    if (!any) return -1;
    for (int i = 58; i < 122 && p[i]; i++) if (p[i] < 32 || p[i] > 126) return -1;
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip)) return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->address, sizeof out->address, "%s:%u", ip, port);
    memcpy(out->identity, p + 24, 32);
    memcpy(out->name, p + 58, 64); /* bounded and NUL-checked above */
    return 0;
}

typedef struct { LmbSocket fd; int port; uint8_t identity[32]; } LmbHomeBeacon;
static inline void *lmb_home_beacon_loop(void *arg) {
    LmbHomeBeacon b = *(LmbHomeBeacon *)arg; free(arg);
    char hostname[64] = "household";
    (void)gethostname(hostname, sizeof hostname - 1); hostname[63] = 0;
    for (char *p = hostname; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '_';
    uint64_t previous = 0;
    for (;;) {
        uint8_t query[LMB_HOME_DISC_QUERY + 1]; struct sockaddr_in peer;
        LmbSocketLength size = sizeof peer;
        int n = (int)recvfrom(b.fd, (char *)query, sizeof query, 0, (struct sockaddr *)&peer, &size);
        if (n < 0) { (void)lmb_socket_error();if (errno == EINTR) continue; break; }
        uint64_t now = lmb_home_disc_ms();
        if (n != LMB_HOME_DISC_QUERY || memcmp(query, "LMBFIND1", 8) ||
            peer.sin_family != AF_INET || now - previous < 100 || !lmb_home_on_link(peer.sin_addr)) continue;
        previous = now;
        uint8_t reply[LMB_HOME_DISC_REPLY] = {0};
        memcpy(reply, "LMBHOME1", 8); memcpy(reply + 8, query + 8, 16);
        memcpy(reply + 24, b.identity, 32);
        reply[56] = (uint8_t)b.port; reply[57] = (uint8_t)(b.port >> 8);
        memcpy(reply + 58, hostname, strlen(hostname));
        (void)sendto(b.fd, (const char *)reply, sizeof reply, 0, (struct sockaddr *)&peer, size);
    }
    lmb_socket_close(b.fd); return NULL;
}

static inline int lmb_home_beacon_start(int port, const uint8_t identity[32]) {
    LmbSocket fd = lmb_socket_open(AF_INET, SOCK_DGRAM, 0);
    if (fd == LMB_INVALID_SOCKET) return -1;
    struct sockaddr_in addr = {0}; addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port); addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr)) { lmb_socket_close(fd); return -1; }
    LmbHomeBeacon *b = malloc(sizeof *b);
    if (!b) { lmb_socket_close(fd); return -1; }
    b->fd = fd; b->port = port; memcpy(b->identity, identity, 32);
    pthread_t thread;
    if (pthread_create(&thread, NULL, lmb_home_beacon_loop, b)) { free(b); lmb_socket_close(fd); return -1; }
    pthread_detach(thread); return 0;
}

static inline int lmb_home_find(LmbHomeFound found[LMB_HOME_DISC_MAX], const uint8_t nonce[16]) {
    int base = lmb_home_port_base(), count = 0;
    if (base < 0) return 0;
    LmbSocket fd = lmb_socket_open(AF_INET, SOCK_DGRAM, 0);int one = 1;
    if (fd == LMB_INVALID_SOCKET) return 0;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one)) { lmb_socket_close(fd); return 0; }
    uint8_t query[LMB_HOME_DISC_QUERY]; memcpy(query, "LMBFIND1", 8); memcpy(query + 8, nonce, 16);
    struct sockaddr_in dst = {0}; dst.sin_family = AF_INET; dst.sin_port = htons((uint16_t)base);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    (void)sendto(fd, (const char *)query, sizeof query, 0, (struct sockaddr *)&dst, sizeof dst);
    /* Same-machine test/development households are also discoverable. */
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)sendto(fd, (const char *)query, sizeof query, 0, (struct sockaddr *)&dst, sizeof dst);
    uint64_t deadline = lmb_home_disc_ms() + 1200;
    while (count < LMB_HOME_DISC_MAX && lmb_home_disc_ms() < deadline) {
        if (lmb_socket_wait(fd,0,100)<=0) continue;
        uint8_t reply[LMB_HOME_DISC_REPLY + 1]; struct sockaddr_in peer; LmbSocketLength len = sizeof peer;
        int n = (int)recvfrom(fd, (char *)reply, sizeof reply, 0, (struct sockaddr *)&peer, &len);
        LmbHomeFound entry;
        if (n < 0 || !lmb_home_on_link(peer.sin_addr) ||
            lmb_home_disc_parse(reply, (size_t)n, query + 8, &peer, &entry)) continue;
        int duplicate = 0;
        for (int i = 0; i < count; i++) if (!memcmp(found[i].identity, entry.identity, 32)) duplicate = 1;
        if (!duplicate) found[count++] = entry;
    }
    lmb_socket_close(fd); return count;
}
#endif
