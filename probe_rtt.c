/* probe_rtt.c — how much of a layer round is the network, and how much is the
 * expert itself? Sends N PINGs (empty round trips) and N EXECs (real expert
 * work) to one node and reports both, so the P2P overhead can be attributed
 * instead of guessed.
 *
 *   ./probe_rtt HOST:PORT LAYER EID HIDDEN [N]
 */
#include "lumibri_proto.h"
#include <time.h>

static double nowd(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s HOST:PORT LAYER EID HIDDEN [N]\n", argv[0]); return 2; }
    const char *addr = argv[1];
    uint32_t layer = (uint32_t)atoi(argv[2]), eid = (uint32_t)atoi(argv[3]);
    uint32_t D = (uint32_t)atoi(argv[4]);
    int N = argc > 5 ? atoi(argv[5]) : 200;

    int fd = lmb_connect(addr);
    if (fd < 0) { fprintf(stderr, "cannot reach %s\n", addr); return 1; }

    double t0 = nowd();
    for (int i = 0; i < N; i++) {
        LmbMsg m = {0};
        if (lmb_send(fd, LMB_PING, NULL, 0, NULL, 0) || lmb_recv(fd, &m)) return 1;
        lmb_msg_free(&m);
    }
    double ping_ms = 1000.0 * (nowd() - t0) / N;

    float *x = calloc(D, sizeof(float));
    for (uint32_t i = 0; i < D; i++) x[i] = 0.01f * (float)((i % 17) - 8);
    LmbBuf b = {0};
    lmb_buf_u32(&b, layer); lmb_buf_u32(&b, eid); lmb_buf_u32(&b, D);

    t0 = nowd();
    for (int i = 0; i < N; i++) {
        LmbMsg m = {0};
        if (lmb_send(fd, LMB_EXEC, b.p, (uint32_t)b.len, x, D * (uint32_t)sizeof(float)) ||
            lmb_recv(fd, &m) || m.op != LMB_EXEC_R) { fprintf(stderr, "exec failed\n"); return 1; }
        lmb_msg_free(&m);
    }
    double exec_ms = 1000.0 * (nowd() - t0) / N;

    printf("ping  %.3f ms   (protocol + TCP round trip, empty)\n", ping_ms);
    printf("exec  %.3f ms   (same round trip carrying %u floats + the expert)\n", exec_ms, D);
    printf("      → expert compute + payload = %.3f ms of it\n", exec_ms - ping_ms);
    free(x); free(b.p); close(fd);
    return 0;
}
