/* One real expert call, direct and through the tracker's NAT tunnel. */
#include "lumabri_proto.h"

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s TRACKER NODE MODEL NEXPERTS DIM EID\n", argv[0]);
        return 2;
    }
    const char *tracker = argv[1], *node = argv[2], *model = argv[3];
    uint32_t nexp = (uint32_t)atoi(argv[4]), dim = (uint32_t)atoi(argv[5]);
    uint32_t eid = (uint32_t)atoi(argv[6]), layer = 0, rows = 3;
    float *x = (float *)malloc((size_t)rows * dim * sizeof(float));
    if (!x) return 2;
    for (size_t i = 0; i < (size_t)rows * dim; i++)
        x[i] = (float)((int)(i % 31) - 15) / 31.0f;

    LmbBuf exec = {0};
    lmb_buf_u32(&exec, layer); lmb_buf_u32(&exec, eid);
    lmb_buf_u32(&exec, dim); lmb_buf_u32(&exec, rows);
    uint32_t pay_len = rows * dim * (uint32_t)sizeof(float);
    LmbMsg direct = {0}, relayed = {0};
    int bad = lmb_request_pay(node, LMB_EXEC, exec.p, (uint32_t)exec.len,
                              x, pay_len, &direct) ||
              direct.op != LMB_EXEC_R || direct.pay_len != pay_len;
    if (!bad) {
        LmbBuf b = {0};
        lmb_buf_str(&b, model); lmb_buf_u32(&b, nexp);
        lmb_buf_bytes(&b, exec.p, exec.len);
        bad = lmb_request_pay(tracker, LMB_REXEC, b.p, (uint32_t)b.len,
                              x, pay_len, &relayed) ||
              relayed.op != LMB_REXEC_R || relayed.pay_len != pay_len ||
              memcmp(direct.pay, relayed.pay, pay_len);
        free(b.p);
    }
    free(exec.p); free(x); lmb_msg_free(&direct); lmb_msg_free(&relayed);
    if (bad) { fprintf(stderr, "relayed EXEC differs from direct EXEC\n"); return 1; }
    puts("EXEC RELAY: PASS (3-row batch, byte-identical to direct)");
    return 0;
}
