/* One real expert call, direct and through the tracker's NAT tunnel —
 * anonymous (REXEC: any holder) and targeted (TEXEC: exactly this peer),
 * plus the peer's manifest over the same tunnel (TMAN). Every answer must
 * be byte-identical to the direct one. */
#include "lumabri_proto.h"

int main(int argc, char **argv) {
    if (argc != 7 && argc != 8) {
        fprintf(stderr, "usage: %s TRACKER NODE MODEL NEXPERTS DIM EID "
                        "[ADVERTISED]\n", argv[0]);
        return 2;
    }
    const char *tracker = argv[1], *node = argv[2], *model = argv[3];
    const char *advertised = argc == 8 ? argv[7] : NULL;
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
    if (!bad && advertised) {
        /* targeted: the tunnel must reach exactly the named peer */
        LmbBuf b = {0};
        LmbMsg targeted = {0};
        lmb_buf_str(&b, advertised);
        lmb_buf_bytes(&b, exec.p, exec.len);
        bad = lmb_request_pay(tracker, LMB_TEXEC, b.p, (uint32_t)b.len,
                              x, pay_len, &targeted) ||
              targeted.op != LMB_EXEC_R || targeted.pay_len != pay_len ||
              memcmp(direct.pay, targeted.pay, pay_len);
        free(b.p); lmb_msg_free(&targeted);
        if (bad) { fprintf(stderr, "targeted TEXEC differs from direct EXEC\n"); }
    }
    if (!bad && advertised) {
        /* and the manifest arrives through the same tunnel, byte-identical */
        LmbMsg dman = {0}, tman = {0};
        LmbBuf b = {0};
        lmb_buf_str(&b, advertised);
        bad = lmb_request(node, LMB_EMANIFEST, NULL, 0, &dman) ||
              dman.op != LMB_EMANIFEST_R ||
              lmb_request(tracker, LMB_TMAN, b.p, (uint32_t)b.len, &tman) ||
              tman.op != LMB_EMANIFEST_R ||
              dman.body_len != tman.body_len ||
              memcmp(dman.body, tman.body, dman.body_len);
        free(b.p); lmb_msg_free(&dman); lmb_msg_free(&tman);
        if (bad) { fprintf(stderr, "tunnelled manifest differs from direct\n"); }
    }
    free(exec.p); free(x); lmb_msg_free(&direct); lmb_msg_free(&relayed);
    if (bad) { fprintf(stderr, "relayed EXEC differs from direct EXEC\n"); return 1; }
    puts(advertised
         ? "EXEC RELAY: PASS (anonymous + targeted + manifest, byte-identical)"
         : "EXEC RELAY: PASS (3-row batch, byte-identical to direct)");
    return 0;
}
