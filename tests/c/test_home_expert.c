/* Exercise the production handler: invalid scopes must never reach a kernel. */
#define main segment_node_program_main
#define lmb_home_expert_apply fixture_expert_apply
#include "../../segment_node.c"
#undef main
#undef lmb_home_expert_apply
#include <assert.h>

static unsigned calls;
static Node *observed_node;
int fixture_expert_apply(int layer, int expert, const float *x, int D, float *out) {
    assert(layer == 2 && expert == 1 && D == 4);
    pthread_mutex_lock(&observed_node->sessions_lock);
    assert(observed_node->control.experts==1);
    pthread_mutex_unlock(&observed_node->sessions_lock);
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
typedef struct { Node *node; uint8_t *body; } QueuedExpert;
static void *queued_expert(void *opaque) {
    QueuedExpert *work=opaque;
    request(work->node,work->body,80,LMB_HOME_EXPERT,LMB_EXEC_R,16);
    return NULL;
}
static void drain_queued_expert(Node *node, uint8_t *body) {
    assert(lmb_run_gate_enter(&node->run_gate,1000,NULL,NULL)==1);
    node->run_wait_ms=3000;
    QueuedExpert work={node,body}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,queued_expert,&work));
    uint64_t deadline=lmb_compute_now()+2000;
    while (!lmb_run_gate_queued(&node->run_gate)) {
        assert(lmb_compute_now()<deadline);
        (void)poll(NULL,0,1);
    }
    pthread_mutex_lock(&node->sessions_lock);
    assert(node->control.experts==1);
    node->control.draining=1;
    pthread_mutex_unlock(&node->sessions_lock);
    unsigned before=calls;
    request(node,body,80,LMB_HOME_EXPERT,LMB_ERR,16); /* new work is refused */
    assert(calls==before);
    lmb_run_gate_leave(&node->run_gate);
    pthread_join(thread,NULL); /* already admitted work still reaches the kernel */
    assert(calls==before+1 && !node->control.experts && !lmb_run_gate_inflight(&node->run_gate));
    node->control.draining=0; node->run_wait_ms=10;
}
static void open_request(Node *node, LmbSegOpen *open, LmbSegStatus expected) {
    int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    LmbMsg msg={.op=LMB_SEG_OPEN},response={0};
    assert(!lmb_seg_open_encode(open,&msg.body,&msg.body_len));
    assert(!handle_open(node,pair[0],&msg));
    assert(!lmb_recv(pair[1],&response) && response.op==LMB_SEG_OPEN_R);
    LmbSegReply reply; assert(!lmb_seg_reply_decode(response.body,response.body_len,&reply));
    assert(reply.status==expected); lmb_msg_free(&msg); lmb_msg_free(&response);
    close(pair[0]); close(pair[1]);
}
static void node_control_test(Node *node) {
    int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    node->control_fd=pair[1]; node->control.instance[0]=7; node->control.revision=1;
    pthread_t control; assert(!pthread_create(&control,NULL,node_control_worker,node));
    uint8_t request_wire[LMB_NODE_CONTROL_REQUEST_BYTES],reply_wire[LMB_NODE_CONTROL_REPLY_BYTES];
    LmbNodeControl current;
    lmb_node_control_request(request_wire,LMB_NODE_CONTROL_QUERY,NULL);
    pthread_mutex_lock(&node->sessions_lock);
    assert(!lmb_node_control_local(&pair[0],request_wire,reply_wire));
    assert(lmb_node_control_decode(reply_wire,sizeof reply_wire,&current)==LMB_NODE_CONTROL_BUSY);
    pthread_mutex_unlock(&node->sessions_lock);
    assert(!lmb_node_control_local(&pair[0],request_wire,reply_wire));
    assert(!lmb_node_control_decode(reply_wire,sizeof reply_wire,&current));
    lmb_node_control_request(request_wire,LMB_NODE_CONTROL_DRAIN,&current);
    assert(!lmb_node_control_local(&pair[0],request_wire,reply_wire));
    assert(!lmb_node_control_decode(reply_wire,sizeof reply_wire,&current) && current.draining);
    node->registration.stop=1; pthread_join(control,NULL);
    close(pair[0]); close(pair[1]); node->registration.stop=0;
    LmbSegOpen open={.session_id={{1}},.request_id={{2}},.owner={.lease_id={{3}},.fencing_epoch=1,.route_generation=1},
        .model_root={2},.tokenizer_root={4},.layer_begin=2,.layer_end=3,.context_tokens=64,
        .max_rows=1,.state_dtype=LMB_SEG_DTYPE_F32,.state_width=4,.ttl_ms=30000,
        .capabilities=LMB_SEG_CAP_CPU|LMB_SEG_CAP_RANGE_NATIVE|LMB_SEG_CAP_MULTI_SESSION|LMB_SEG_CAP_TOKEN_IDS};
    strcpy(open.engine_id,"olmoe"); strcpy(open.state_schema,"kv-standard-v1"); strcpy(open.numeric_class,"fixture");
    node->registration.ready=1; node->registration.owner=open.owner;
    node->advert.tokenizer_root[0]=4; node->advert.max_context=64; node->advert.max_rows=1;
    node->advert.state_dtype=open.state_dtype; node->advert.state_width=4; node->advert.capabilities=open.capabilities;
    strcpy(node->advert.engine_id,open.engine_id); strcpy(node->advert.state_schema,open.state_schema);
    strcpy(node->advert.numeric_class,open.numeric_class);
    node->table=lmb_seg_table_create(2); assert(node->table);
    open_request(node,&open,LMB_SEG_STATUS_QUOTA); /* No engine exists: must reject BEFORE allocation. */
    assert(lmb_seg_table_open(node->table,&open,now_ms())==LMB_SEG_STATUS_OK);
    node->sessions[0].used=1; node->sessions[0].id=open.session_id;
    open_request(node,&open,LMB_SEG_STATUS_DUPLICATE); /* live conversation retry is still allowed */
    assert(!run_should_cancel(node)); /* drain alone does not abort admitted kernels */
    node->sessions[0].used=0; lmb_seg_table_destroy(node->table); node->table=NULL;
    node->control.draining=0;
    puts("NODE ADMISSION: PASS (private channel, busy is unknown, new OPEN refused, existing OPEN retained, no drain cancellation)");
}
int main(void) {
    Node *node = calloc(1, sizeof *node); assert(node);
    observed_node=node;
    assert(!pthread_mutex_init(&node->sessions_lock,NULL));
    assert(!pthread_mutex_init(&node->registration.lock,NULL));
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
    node->control.draining=1;
    request(node,body,80,LMB_HOME_EXPERT,LMB_ERR,16);
    assert(!calls && !node->control.experts);
    node->control.draining=0;
    request(node, body, 80, LMB_HOME_EXPERT, LMB_EXEC_R, 16);
    assert(calls == 1 && atomic_load(&node->expert_runs) == 1);
    assert(!lmb_run_gate_inflight(&node->run_gate));
    drain_queued_expert(node,body);
    memset(body+64, 0, 64); strcpy((char *)body+64, node->cap.numeric_class);
    request(node, body, 128, LMB_HOME_FEATURES, LMB_OK, 0);
    body[64] ^= 1; request(node, body, 128, LMB_HOME_FEATURES, 0, 0);
    assert(calls == 2);
    assert(!node->control.experts);
    node_control_test(node);
    lmb_run_gate_destroy(&node->run_gate);
    pthread_mutex_destroy(&node->sessions_lock); pthread_mutex_destroy(&node->registration.lock); free(node);
    puts("HOME EXPERT: PASS (approval, allocation, root, layer, shape, numeric class, admission)");
    return 0;
}
