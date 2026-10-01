/* Exercise the production handler: invalid scopes must never reach a kernel. */
#define main segment_node_program_main
#define lmb_home_expert_apply fixture_expert_apply
#include "../../segment_node.c"
#undef main
#undef lmb_home_expert_apply
#include <assert.h>

static unsigned calls;
int fixture_expert_apply(int layer, int expert, const float *x, int D, float *out) {
    assert(layer == 2 && expert == 1 && D == 4);
    calls++;
    for (int i = 0; i < D; i++) out[i] = x[i] * 2;
    return 0;
}
static void request(Node *node, uint8_t *body, uint32_t size, uint32_t op,
                    uint32_t expected, uint32_t payload_size) {
    float x[4] = {1, 2, 3, 4};
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    LmbMsg msg = {.op=op, .body=body, .body_len=size,
                 .pay=(uint8_t *)x, .pay_len=payload_size};
    int rc = handle_home_expert(node, pair[0], &msg);
    if (expected) {
        assert(!rc); LmbMsg response = {0}; assert(!lmb_recv(pair[1], &response));
        assert(response.op == expected);
        if (expected == LMB_EXEC_R) {
            assert(response.pay_len == sizeof x);
            for (unsigned i = 0; i < 4; i++) assert(((float *)response.pay)[i] == x[i] * 2);
        }
        lmb_msg_free(&response);
    } else assert(rc);
    close(pair[0]); close(pair[1]);
}
int main(void) {
    Node *node = calloc(1, sizeof *node); assert(node);
    assert(!lmb_run_gate_init(&node->run_gate, 1, 1));
    node->home_allocation[0] = 1; node->advert.model_root[0] = 2;
    node->advert.layer_begin = 2; node->advert.layer_end = 3;
    node->cap.state_width = 4; node->run_wait_ms = 10;
    strcpy(node->cap.numeric_class, "olmoe/f32-int8/cpu-v1");
    uint8_t body[128] = {0}; body[0] = 1; body[32] = 2;
    lmb_put32(body+64, 2); lmb_put32(body+68, 1);
    lmb_put32(body+72, 4); lmb_put32(body+76, 1);
    request(node, body, 80, LMB_HOME_EXPERT, LMB_ERR, 16); /* no accelerator approval */
    node->home_accelerator = 1;
    body[0] = 9; request(node, body, 80, LMB_HOME_EXPERT, LMB_ERR, 16); body[0] = 1;
    body[32] = 9; request(node, body, 80, LMB_HOME_EXPERT, LMB_ERR, 16); body[32] = 2;
    lmb_put32(body+64, 1); request(node, body, 80, LMB_HOME_EXPERT, LMB_ERR, 16);
    lmb_put32(body+64, 3); request(node, body, 80, LMB_HOME_EXPERT, LMB_ERR, 16);
    lmb_put32(body+64, 2); lmb_put32(body+76, 2);
    request(node, body, 80, LMB_HOME_EXPERT, LMB_ERR, 16); lmb_put32(body+76, 1);
    request(node, body, 80, LMB_HOME_EXPERT, LMB_ERR, 12);
    request(node, body, 79, LMB_HOME_EXPERT, LMB_ERR, 16);
    assert(!calls);
    request(node, body, 80, LMB_HOME_EXPERT, LMB_EXEC_R, 16);
    assert(calls == 1 && atomic_load(&node->expert_runs) == 1);
    assert(!lmb_run_gate_inflight(&node->run_gate));
    memset(body+64, 0, 64); strcpy((char *)body+64, node->cap.numeric_class);
    request(node, body, 128, LMB_HOME_FEATURES, LMB_OK, 0);
    body[64] ^= 1; request(node, body, 128, LMB_HOME_FEATURES, 0, 0);
    assert(calls == 1);
    lmb_run_gate_destroy(&node->run_gate); free(node);
    puts("HOME EXPERT: PASS (approval, allocation, root, layer, shape, numeric class, admission)");
    return 0;
}
