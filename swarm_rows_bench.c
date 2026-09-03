/* How much does a layer round cost when it carries B rows instead of one?
 *
 * Speculative decoding verifies B candidate tokens in one pass: every
 * expert call carries B activations, the number of rounds is unchanged.
 * Whether that pays on a given network is this ratio — round(B) / round(1)
 * — times the acceptance the drafter achieves. This tool measures the
 * ratio against a real executor: six experts of one layer, called in
 * parallel like a chatter does, with 1, 2, 4, 8 and 16 rows each, bf16
 * on the wire when the node speaks EXEC2.
 *
 *   ./swarm_rows_bench ADDR LAYER HIDDEN            (experts 0..5 of LAYER)
 */
#include "lumabri_proto.h"
#include <pthread.h>
#include <math.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

typedef struct {
    const char *addr; int layer, eid, D, rows, exec2;
    const float *x; const float *w;
    double ms; int ok; uint32_t up, down;
} Call;

static void *one_call(void *arg) {
    Call *c = arg;
    c->ok = 0;
    int fd = lmb_connect(c->addr);
    if (fd < 0 || lmb_auth(fd)) return NULL;
    size_t n = (size_t)c->rows * c->D;
    LmbBuf b = {0};
    lmb_buf_u32(&b, (uint32_t)c->layer); lmb_buf_u32(&b, (uint32_t)c->eid);
    lmb_buf_u32(&b, (uint32_t)c->D); lmb_buf_u32(&b, (uint32_t)c->rows);
    int bf16 = c->exec2 && lmb_bf16_exact(c->x, n);
    if (c->exec2) lmb_buf_u32(&b, bf16 ? LMB_ENC_BF16 : LMB_ENC_F32);
    lmb_buf_bytes(&b, c->w, (size_t)c->rows * sizeof(float));
    uint16_t *packed = NULL; const void *pay = c->x; uint32_t plen = (uint32_t)(n * 4);
    if (bf16) { packed = malloc(n * 2); lmb_bf16_pack(packed, c->x, n); pay = packed; plen = (uint32_t)(n * 2); }
    double t0 = now_s();
    LmbMsg m = {0};
    if (!lmb_send(fd, c->exec2 ? LMB_EXEC2 : LMB_EXEC, b.p, (uint32_t)b.len, pay, plen) &&
        !lmb_recv(fd, &m) && (m.op == LMB_EXEC_R || m.op == LMB_EXEC2_R) && m.pay_len) {
        c->ms = (now_s() - t0) * 1000.0; c->ok = 1;
        c->up = 16 + (uint32_t)b.len + plen; c->down = 16 + m.pay_len;
    } else if (m.op == LMB_ERR) {
        char why[200] = ""; LmbCur cur = { m.body, m.body_len, 0 }; lmb_cur_str(&cur, why, sizeof why);
        fprintf(stderr, "  expert %d: refused: %s\n", c->eid, why);
    }
    lmb_msg_free(&m); free(b.p); free(packed); close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s ADDR LAYER HIDDEN\n", argv[0]); return 2; }
    const char *addr = argv[1];
    int layer = atoi(argv[2]), D = atoi(argv[3]);
    /* does the node speak EXEC2? ask like the chatter does */
    int exec2 = 0;
    { LmbMsg rm = {0};
      if (!lmb_request(addr, LMB_ERES, NULL, 0, &rm) && rm.op == LMB_ERES_R) {
          LmbCur c = { rm.body, rm.body_len, 0 }; uint32_t f = 0, st = 0, caps = 0;
          if (!lmb_cur_u32(&c, &f) && !lmb_cur_u32(&c, &st) && !lmb_cur_u32(&c, &caps)) exec2 = (caps & LMB_CAP_EXEC2) != 0;
      }
      lmb_msg_free(&rm); }
    printf("executor %s · layer %d · hidden %d · %s\n", addr, layer, D,
           exec2 ? "EXEC2 (bf16 on the wire)" : "plain EXEC (float32)");
    int rows_list[] = { 1, 2, 4, 8, 16 };
    double base = 0;
    printf("%5s  %10s  %10s  %9s  %8s\n", "rows", "round ms", "per call", "KB up/rnd", "vs 1 row");
    for (size_t ri = 0; ri < sizeof rows_list / sizeof *rows_list; ri++) {
        int rows = rows_list[ri];
        size_t n = (size_t)rows * D;
        float *x = malloc(n * 4), *w = malloc((size_t)rows * 4);
        for (size_t i = 0; i < n; i++) {           /* bf16-exact pseudo activations */
            float v = (float)((int)((i * 7919u) % 2001u) - 1000) / 777.0f;
            uint32_t bits; memcpy(&bits, &v, 4); bits &= 0xFFFF0000u; memcpy(&x[i], &bits, 4);
        }
        for (int r = 0; r < rows; r++) w[r] = 1.0f;
        double best = 1e9, sum_call = 0; int calls = 0; uint32_t up = 0;
        for (int rep = 0; rep < 5; rep++) {         /* five rounds, keep the best */
            Call c[6]; pthread_t t[6];
            for (int e = 0; e < 6; e++) {
                c[e] = (Call){ addr, layer, e, D, rows, exec2, x, w, 0, 0, 0, 0 };
                pthread_create(&t[e], NULL, one_call, &c[e]);
            }
            double worst = 0; int all = 1; uint32_t u = 0;
            for (int e = 0; e < 6; e++) {
                pthread_join(t[e], NULL);
                if (!c[e].ok) all = 0;
                if (c[e].ms > worst) worst = c[e].ms;
                sum_call += c[e].ms; calls++; u += c[e].up;
            }
            if (all && worst < best) { best = worst; up = u; }
        }
        if (best >= 1e9) { printf("%5d  %10s\n", rows, "failed"); free(x); free(w); continue; }
        if (!base) base = best;
        printf("%5d  %10.1f  %10.1f  %9.0f  %7.2fx\n", rows, best, sum_call / calls,
               up / 1000.0, best / base);
        free(x); free(w);
    }
    printf("\nround(B)/round(1) is the price of an ondata of B: with acceptance a per\n"
           "proposal and B-1 proposals, tokens per round = 1 + a*(B-1), so the\n"
           "speed-up is (1 + a*(B-1)) / ratio.\n");
    return 0;
}
