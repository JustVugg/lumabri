/* Fixed household service range. No random port may escape the firewall
 * allowance. The override is for isolated tests/advanced deployments. */
#ifndef LUMABRI_HOME_NET_H
#define LUMABRI_HOME_NET_H
#include "lumabri_proto.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <limits.h>

#define LMB_HOME_PORT_BASE 47300
#define LMB_HOME_PORT_COUNT 16

/* Consume a socket reserved by our launcher. Holding it across exec avoids
 * two concurrently approved donors choosing the same apparently free port.
 * Restore CLOEXEC before any engine descendants are launched. */
static inline int lmb_home_take_listener(int port) {
    const char *value = getenv("LUMABRI_HOME_LISTEN_FD");
    if (!value) {
        int fd = lmb_listen(port);
        if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC)) {
            int saved = errno; close(fd); errno = saved; return -1;
        }
        return fd;
    }
    char *end; errno = 0;
    long n = strtol(value, &end, 10);
    int bad = errno || !*value || *end || n < 3 || n > INT_MAX;
    unsetenv("LUMABRI_HOME_LISTEN_FD");
    if (bad) { errno = EINVAL; return -1; }
    int fd = (int)n, listening = 0;
    struct sockaddr_in addr = {0}; socklen_t size = sizeof addr, opt = sizeof listening;
    if (getsockname(fd, (struct sockaddr *)&addr, &size) || size != sizeof addr ||
        addr.sin_family != AF_INET || ntohs(addr.sin_port) != port ||
        getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &opt) || !listening ||
        fcntl(fd, F_SETFD, FD_CLOEXEC)) { close(fd); errno = EINVAL; return -1; }
    return fd;
}

static inline int lmb_home_port_base(void) {
    const char *s = getenv("LUMABRI_HOME_PORT_BASE");
    if (!s || !*s) return LMB_HOME_PORT_BASE;
    char *end; errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || *end || n < 1024 || n > 65536 - LMB_HOME_PORT_COUNT) {
        errno = EINVAL; return -1;
    }
    return (int)n;
}

static inline int lmb_home_listen_service(int *port) {
    int base = lmb_home_port_base();
    if (base < 0) return -1;
    for (int n = base + 1; n < base + LMB_HOME_PORT_COUNT; n++) {
        int fd = lmb_listen(n);
        if (fd < 0) {
            if (errno == EADDRINUSE) continue;
            return -1;
        }
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            int saved = errno; close(fd); errno = saved; return -1;
        }
        *port = n; return fd;
    }
    errno = EADDRINUSE; return -1;
}

static inline int lmb_home_ip_score(unsigned flags, uint32_t host) {
    if (!(flags & IFF_UP) || (flags & IFF_LOOPBACK) || !host ||
        (host >> 24) == 127 || (host >> 24) == 0 ||
        (host >> 16) == 0xa9fe || (host >> 28) >= 14) return 0;
    int private_ip = (host >> 24) == 10 || (host >> 20) == 0xac1 ||
                     (host >> 16) == 0xc0a8;
    return 1 + private_ip * 2 + !(flags & IFF_POINTOPOINT);
}

static inline int lmb_home_interface_ip(char *out, size_t cap) {
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all)) return -1;
    int best = 0;
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *a = (struct sockaddr_in *)p->ifa_addr;
        int score = lmb_home_ip_score(p->ifa_flags, ntohl(a->sin_addr.s_addr));
        if (score > best && inet_ntop(AF_INET, &a->sin_addr, out, cap)) best = score;
    }
    freeifaddrs(all);
    return best ? 0 : -1;
}

static inline int lmb_home_subnet(const char *ip, char *out, size_t cap) {
    struct in_addr wanted;
    if (inet_pton(AF_INET, ip, &wanted) != 1) return -1;
    struct ifaddrs *all = NULL; int rc = -1;
    if (getifaddrs(&all)) return -1;
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET ||
            ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr != wanted.s_addr) continue;
        uint32_t mask = ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
        unsigned prefix = 0; uint32_t bits = mask;
        while (bits & 0x80000000u) { prefix++; bits <<= 1; }
        if (bits || prefix < 8 || prefix > 30) continue;
        struct in_addr network = {htonl(ntohl(wanted.s_addr) & mask)};
        char text[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &network, text, sizeof text)) {
            int n = snprintf(out, cap, "%s/%u", text, prefix);
            rc = n >= 0 && (size_t)n < cap ? 0 : -1;
        }
    }
    freeifaddrs(all); return rc;
}
#endif
