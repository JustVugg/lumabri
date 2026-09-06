#define _GNU_SOURCE
#include "lumabri_home_discovery.h"
#include "src/ui/lumabri_visual.h"
#include <assert.h>

int main(void) {
    unsigned up = IFF_UP;
    assert(!lmb_home_ip_score(up | IFF_LOOPBACK, 0x0afffffe)); /* WSL DNS alias */
    assert(!lmb_home_ip_score(0, 0xc0a8010d));
    assert(!lmb_home_ip_score(up, 0x7f000001));
    assert(!lmb_home_ip_score(up, 0xa9fe0001));
    assert(!lmb_home_ip_score(up, 0xe0000001));
    assert(lmb_home_ip_score(up, 0xc0a8010d) > lmb_home_ip_score(up | IFF_POINTOPOINT, 0x0a000001));
    unsetenv("LUMABRI_HOME_PORT_BASE"); assert(lmb_home_port_base() == 47300);
    setenv("LUMABRI_HOME_PORT_BASE", "0", 1); assert(lmb_home_port_base() < 0);
    setenv("LUMABRI_HOME_PORT_BASE", "65521", 1); assert(lmb_home_port_base() < 0);
    setenv("LUMABRI_HOME_PORT_BASE", "47300oops", 1); assert(lmb_home_port_base() < 0);

    int fds[16], base = 0;
    for (int attempt = 0; attempt < 40 && !base; attempt++) {
        int candidate = 50000 + ((getpid() + attempt) % 800) * 16, count = 0;
        for (; count < 16; count++) { fds[count] = lmb_listen(candidate + count); if (fds[count] < 0) break; }
        for (int i = 0; i < count; i++) close(fds[i]);
        if (count == 16) base = candidate;
    }
    assert(base);
    char setting[20]; snprintf(setting, sizeof setting, "%d", base);
    setenv("LUMABRI_HOME_PORT_BASE", setting, 1);
    for (int i = 0; i < 15; i++) {
        int port = 0; fds[i] = lmb_home_listen_service(&port);
        assert(fds[i] >= 0 && port == base + i + 1);
        assert(fcntl(fds[i], F_GETFD) & FD_CLOEXEC);
    }
    int port = 0;
    assert(lmb_home_listen_service(&port) < 0); /* never spill to random ports */
    for (int i = 0; i < 15; i++) close(fds[i]);
    int restarted = lmb_home_listen_service(&port);
    assert(restarted >= 0 && port == base + 1);
    char descriptor[32]; snprintf(descriptor, sizeof descriptor, "%d", restarted);
    assert(!fcntl(restarted, F_SETFD, 0));
    setenv("LUMABRI_HOME_LISTEN_FD", descriptor, 1);
    assert(lmb_home_take_listener(port) == restarted);
    assert(!getenv("LUMABRI_HOME_LISTEN_FD"));
    assert(fcntl(restarted, F_GETFD) & FD_CLOEXEC);
    setenv("LUMABRI_HOME_LISTEN_FD", descriptor, 1);
    assert(lmb_home_take_listener(port + 1) < 0); /* wrong port fails closed */
    assert(fcntl(restarted, F_GETFD) < 0);
    setenv("LUMABRI_HOME_LISTEN_FD", "1", 1);
    assert(lmb_home_take_listener(port) < 0);
    assert(fcntl(1, F_GETFD) >= 0); /* malformed env must not close stdout */

    uint8_t nonce[16] = {3}, reply[LMB_HOME_DISC_REPLY] = {0};
    memcpy(reply, "LMBHOME1", 8); memcpy(reply + 8, nonce, 16);
    reply[24] = 9; reply[56] = (uint8_t)base; reply[57] = (uint8_t)(base >> 8);
    memcpy(reply + 58, "test-house", 10);
    struct sockaddr_in peer = {0}; peer.sin_family = AF_INET; peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    LmbHomeFound found;
    assert(!lmb_home_disc_parse(reply, sizeof reply, nonce, &peer, &found));
    for (size_t n = 0; n < sizeof reply; n++) assert(lmb_home_disc_parse(reply, n, nonce, &peer, &found));
    reply[8] ^= 1; assert(lmb_home_disc_parse(reply, sizeof reply, nonce, &peer, &found)); reply[8] ^= 1;
    reply[58] = 27; assert(lmb_home_disc_parse(reply, sizeof reply, nonce, &peer, &found)); reply[58] = 't';
    memset(reply + 58, 'x', 64); assert(lmb_home_disc_parse(reply, sizeof reply, nonce, &peer, &found));

    uint8_t identity[32] = {9};
    assert(!lmb_home_beacon_start(base, identity));
    LmbHomeFound results[LMB_HOME_DISC_MAX];
    int n = lmb_home_find(results, nonce), matched = 0;
    for (int i = 0; i < n; i++) if (!memcmp(results[i].identity, identity, 32)) matched = 1;
    assert(matched);

    unsetenv("NO_COLOR"); unsetenv("LUMABRI_COLOR");
    setenv("TERM_PROGRAM", "Apple_Terminal", 1);
    setenv("COLORTERM", "truecolor", 1); /* inherited value must not break Apple Terminal */
    assert(ui_color_mode() == 256);
    setenv("TERM_PROGRAM", "iTerm.app", 1); assert(ui_color_mode() == 24);
    unsetenv("COLORTERM"); setenv("TERM", "xterm-256color", 1); assert(ui_color_mode() == 256);
    setenv("TERM", "vt100", 1); assert(ui_color_mode() == 16);
    setenv("NO_COLOR", "1", 1); assert(ui_color_mode() == 0);
    puts("HOME NETWORK: PASS (loopback exclusion, bounded ports, restart, discovery validation, terminal palette)");
    return 0;
}
