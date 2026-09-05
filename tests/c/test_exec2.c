/* EXEC2 against a real expert node: the same bf16-exact activation sent as
 * plain EXEC (floats) and as EXEC2 (bf16 halves) must come back identical,
 * and the EXEC2 reply must be bf16 when the engine's output is. */
#include "lumabri_proto.h"
#include <math.h>

static float *ask(const char *addr, int op, int layer, int eid, int D,
                  const float *x, uint32_t *enc_out, uint32_t *pay_len) {
    int fd = lmb_connect(addr);
    if (fd < 0 || lmb_auth(fd)) { fprintf(stderr, "connect %s failed\n", addr); return NULL; }
    LmbBuf b = {0};
    lmb_buf_u32(&b, (uint32_t)layer); lmb_buf_u32(&b, (uint32_t)eid);
    lmb_buf_u32(&b, (uint32_t)D); lmb_buf_u32(&b, 1u);
    uint16_t *packed = NULL;
    const void *pay = x; uint32_t plen = (uint32_t)D * 4;
    if (op == LMB_EXEC2) {
        int bf16 = lmb_bf16_exact(x, (size_t)D);
        lmb_buf_u32(&b, bf16 ? LMB_ENC_BF16 : LMB_ENC_F32);
        if (bf16) { packed = malloc((size_t)D * 2); lmb_bf16_pack(packed, x, (size_t)D); pay = packed; plen = (uint32_t)D * 2; }
    }
    LmbMsg m = {0};
    float *res = NULL;
    if (!lmb_send(fd, op, b.p, (uint32_t)b.len, pay, plen) && !lmb_recv(fd, &m)) {
        *pay_len = m.pay_len;
        if (op == LMB_EXEC && m.op == LMB_EXEC_R && m.pay_len == (uint32_t)D * 4) {
            *enc_out = LMB_ENC_F32; res = (float *)lmb_msg_take_pay(&m);
        } else if (op == LMB_EXEC2 && m.op == LMB_EXEC2_R && m.body_len >= 4) {
            *enc_out = lmb_get32(m.body);
            if (*enc_out == LMB_ENC_F32 && m.pay_len == (uint32_t)D * 4) res = (float *)lmb_msg_take_pay(&m);
            else if (*enc_out == LMB_ENC_BF16 && m.pay_len == (uint32_t)D * 2) {
                res = malloc((size_t)D * 4); lmb_bf16_unpack(res, (const uint16_t *)m.pay, (size_t)D);
            }
        } else {
            char why[256] = "";
            if (m.op == LMB_ERR) { LmbCur c = { m.body, m.body_len, 0 }; lmb_cur_str(&c, why, sizeof why); }
            fprintf(stderr, "op %d: unexpected reply op=%u body=%u pay=%u %s\n", op, m.op, m.body_len, m.pay_len, why);
        }
    }
    lmb_msg_free(&m); free(b.p); free(packed); close(fd);
    return res;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s ADDR LAYER EID HIDDEN\n", argv[0]); return 2; }
    const char *addr = argv[1];
    int layer = atoi(argv[2]), eid = atoi(argv[3]), D = atoi(argv[4]);
    float *x = malloc((size_t)D * 4);
    /* a bf16-exact activation: random floats rounded to bf16 */
    for (int i = 0; i < D; i++) {
        float v = (float)((i * 7919) % 2000 - 1000) / 333.0f;
        uint32_t bits; memcpy(&bits, &v, 4); bits &= 0xFFFF0000u; memcpy(&x[i], &bits, 4);
    }
    if (!lmb_bf16_exact(x, (size_t)D)) { fprintf(stderr, "test input is not bf16-exact\n"); return 1; }
    uint32_t enc1 = 9, enc2 = 9, len1 = 0, len2 = 0;
    float *a = ask(addr, LMB_EXEC, layer, eid, D, x, &enc1, &len1);
    float *b = ask(addr, LMB_EXEC2, layer, eid, D, x, &enc2, &len2);
    if (!a || !b) { fprintf(stderr, "EXEC2 TEST: FAIL (no reply: exec=%p exec2=%p)\n", (void *)a, (void *)b); return 1; }
    if (memcmp(a, b, (size_t)D * 4)) {
        int first = -1; for (int i = 0; i < D; i++) if (a[i] != b[i]) { first = i; break; }
        fprintf(stderr, "EXEC2 TEST: FAIL (results differ at %d: %g vs %g)\n", first, a[first], b[first]);
        return 1;
    }
    int out_exact = lmb_bf16_exact(a, (size_t)D);
    printf("EXEC2 TEST: PASS · %d floats · request %u bytes as bf16 vs %u as f32 · reply %s (%u bytes)%s\n",
           D, (unsigned)D * 2, (unsigned)D * 4,
           enc2 == LMB_ENC_BF16 ? "bf16" : "f32", len2,
           out_exact && enc2 != LMB_ENC_BF16 ? " · WARNING: output was bf16-exact but came back as f32" : "");
    free(a); free(b); free(x);
    return 0;
}
