/* Deterministic socket-option contract plus an informational loopback A/B.
 * Fixed keys exercise real AEAD records, NOT handshake authentication or a
 * model. Timing is reported, never used as a flaky pass/fail threshold. */
#include "lumabri_secure.h"
#include <assert.h>
#include <pthread.h>
#include <sys/un.h>

#define ROUNDS 24
typedef struct { int fd; LmbSecure secure; } Echo;

static void *echo_records(void *arg) {
    Echo *echo = arg;
    for (unsigned i = 0; i < ROUNDS; i++) {
        LmbMsg message = {0};
        assert(!lmb_secure_recv(&echo->secure, echo->fd, &message));
        assert(message.op == LMB_SEG_RUN);
        assert(!lmb_secure_send(&echo->secure, echo->fd, LMB_SEG_RUN_R,
            message.body, message.body_len, message.pay, message.pay_len));
        lmb_msg_free(&message);
    }
    return NULL;
}

static int nodelay(int fd) {
    int value = -1; socklen_t size = sizeof value;
    assert(!getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &size));
    return value;
}

static double seconds(void) {
    struct timespec ts; assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double trial(int legacy) {
    int listener = lmb_listen(0); assert(listener >= 0);
    struct sockaddr_in bound; socklen_t length = sizeof bound;
    assert(!getsockname(listener, (struct sockaddr *)&bound, &length));
    char address[64]; snprintf(address, sizeof address, "127.0.0.1:%u", ntohs(bound.sin_port));
    int client = lmb_connect_ms_io(address, 1000, 5000); assert(client >= 0);
    int server = accept(listener, NULL, NULL); assert(server >= 0);
    close(listener);
    lmb_set_io_timeout(server, 5000);
    assert(nodelay(client) == 1);
    int zero = 0;
    assert(!setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &zero, sizeof zero));
    assert(nodelay(server) == 0);
    assert(!lmb_secure_server(server));  /* actual accepted-socket entry point */
    assert(nodelay(server) == 1);
    if (legacy) assert(!setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &zero, sizeof zero));
    Echo echo = {.fd = server, .secure = {.active = 1}};
    LmbSecure outgoing = {.active = 1};
    memset(outgoing.tx_key, 0x11, 32); memset(echo.secure.rx_key, 0x11, 32);
    memset(outgoing.rx_key, 0x22, 32); memset(echo.secure.tx_key, 0x22, 32);
    pthread_t thread; assert(!pthread_create(&thread, NULL, echo_records, &echo));
    const char body[] = "test-only framing";
    unsigned char payload[256]; memset(payload, 0x53, sizeof payload);
    double start = seconds();
    for (unsigned i = 0; i < ROUNDS; i++) {
        assert(!lmb_secure_send(&outgoing, client, LMB_SEG_RUN,
                               body, sizeof body, payload, sizeof payload));
        LmbMsg message = {0};
        assert(!lmb_secure_recv(&outgoing, client, &message));
        assert(message.op == LMB_SEG_RUN_R && message.body_len == sizeof body &&
               message.pay_len == sizeof payload);
        assert(!memcmp(message.body, body, sizeof body));
        assert(!memcmp(message.pay, payload, sizeof payload));
        lmb_msg_free(&message);
    }
    double elapsed = seconds() - start;
    assert(!pthread_join(thread, NULL));
    lmb_close(server); lmb_close(client);
    return elapsed * 1000 / ROUNDS;
}

int main(void) {
    (void)lmb_sign;
    (void)lmb_secure_init;
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(!lmb_secure_server(pair[0])); /* Unix sockets have no Nagle option. */
    close(pair[0]); close(pair[1]);
    double legacy = trial(1), current = trial(0);
    assert(lmb_secure_server(-1) < 0);
    printf("INBOUND TCP: PASS (client/server NODELAY; Unix socket preserved; encrypted record equality)\n");
    printf("Informational %u-record loopback mean: legacy %.3f ms, NODELAY %.3f ms; not model tok/s\n",
           ROUNDS, legacy, current);
    return 0;
}
