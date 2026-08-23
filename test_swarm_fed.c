/* The same expert, the same activation rows, two nodes: one reading the
 * container from disk, one fed through the shim's swarm mirror with no model
 * on its disk at all. The replies must be byte-identical — anything else
 * means a swarm-fed compute donor would poison phase 2. */
#include "lumabri_proto.h"

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s NODE_LOCAL NODE_FED DIM LAYER EID\n", argv[0]);
        return 2;
    }
    const char *node_a = argv[1], *node_b = argv[2];
    uint32_t dim = (uint32_t)atoi(argv[3]), layer = (uint32_t)atoi(argv[4]);
    uint32_t eid = (uint32_t)atoi(argv[5]), rows = 3;
    float *x = (float *)malloc((size_t)rows * dim * sizeof(float));
    if (!x) return 2;
    for (size_t i = 0; i < (size_t)rows * dim; i++)
        x[i] = (float)((int)(i % 31) - 15) / 31.0f;

    LmbBuf exec = {0};
    lmb_buf_u32(&exec, layer); lmb_buf_u32(&exec, eid);
    lmb_buf_u32(&exec, dim); lmb_buf_u32(&exec, rows);
    uint32_t pay_len = rows * dim * (uint32_t)sizeof(float);
    LmbMsg a = {0}, b = {0};
    int bad = lmb_request_pay(node_a, LMB_EXEC, exec.p, (uint32_t)exec.len,
                              x, pay_len, &a) ||
              a.op != LMB_EXEC_R || a.pay_len != pay_len;
    if (bad) { fprintf(stderr, "no EXEC reply from the local node\n"); return 1; }
    bad = lmb_request_pay(node_b, LMB_EXEC, exec.p, (uint32_t)exec.len,
                          x, pay_len, &b) ||
          b.op != LMB_EXEC_R || b.pay_len != pay_len;
    if (bad) { fprintf(stderr, "no EXEC reply from the swarm-fed node\n"); return 1; }
    if (memcmp(a.pay, b.pay, pay_len)) {
        fprintf(stderr, "swarm-fed EXEC differs from the local container's\n");
        return 1;
    }
    free(exec.p); free(x); lmb_msg_free(&a); lmb_msg_free(&b);
    puts("SWARM-FED EXEC: PASS (byte-identical to the local container)");
    return 0;
}
