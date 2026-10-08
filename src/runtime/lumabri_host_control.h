/* Authenticated control of an already approved host, never authority to load
 * or unload weights. Mutations are fenced by process instance and revision.
 * The host pool mutex serializes this state with turn admission. */
#ifndef LMB_HOST_CONTROL_H
#define LMB_HOST_CONTROL_H
#define LMB_HOST_CONTROL_VERSION 1u
#define LMB_HOST_CONTROL_QUERY 0u
#define LMB_HOST_CONTROL_DRAIN 1u
#define LMB_HOST_CONTROL_RESUME 2u
#define LMB_HOST_CONTROL_RETIRE 3u
#define LMB_HOST_CONTROL_RETIRED 2u
#define LMB_HOST_CONTROL_CONFLICT 1u
#define LMB_HOST_CONTROL_REQUEST_BYTES 80u
#define LMB_HOST_CONTROL_REPLY_BYTES 92u
typedef struct {
    uint8_t instance[32];
    uint64_t revision;
    uint32_t draining, connections, requests;
} LmbHostControl;

static int lmb_host_control_apply(LmbHostControl *state, uint32_t action,
                                  const uint8_t instance[32], uint64_t expected) {
    if (action==LMB_HOST_CONTROL_QUERY) return 0;
    if (action>LMB_HOST_CONTROL_RETIRE || !expected ||
        memcmp(instance,state->instance,32)) return -1;
    uint32_t draining=action==LMB_HOST_CONTROL_RETIRE ? LMB_HOST_CONTROL_RETIRED : action==LMB_HOST_CONTROL_DRAIN;
    /* A lost reply can be retried with the same fence, but never across an
     * intervening resume/drain or a replacement host process. */
    if (state->revision>1 && expected==state->revision-1 && state->draining==draining) return 0;
    if (state->draining==LMB_HOST_CONTROL_RETIRED ||
        (action==LMB_HOST_CONTROL_RETIRE && (!state->draining || state->connections || state->requests))) return -1;
    if (expected!=state->revision || state->revision==UINT64_MAX) return -1;
    state->draining=draining; state->revision++;
    return 0;
}

static int lmb_host_control_request(LmbBuf *body, uint32_t action, const uint8_t root[32],
                                    const LmbHostControl *expected) {
    uint8_t zero[32]={0};
    return lmb_buf_u32(body,LMB_HOST_CONTROL_VERSION) || lmb_buf_u32(body,action) ||
        lmb_buf_bytes(body,root,32) || lmb_buf_bytes(body,expected ? expected->instance : zero,32) ||
        lmb_buf_u64(body,expected ? expected->revision : 0);
}
static int lmb_host_control_reply(LmbBuf *body, uint32_t status, const uint8_t root[32],
                                  const LmbHostControl *state) {
    return lmb_buf_u32(body,LMB_HOST_CONTROL_VERSION) || lmb_buf_u32(body,status) ||
        lmb_buf_bytes(body,root,32) || lmb_buf_bytes(body,state->instance,32) ||
        lmb_buf_u64(body,state->revision) || lmb_buf_u32(body,state->draining) ||
        lmb_buf_u32(body,state->connections) || lmb_buf_u32(body,state->requests);
}
/* Positive conflict is an authenticated reply, negative means unknown: an
 * old/offline host or invalid response must NEVER be assumed drained. */
static int lmb_host_control_rpc(const char *address, const char *key_text, const char *root_text,
                                uint32_t action, const LmbHostControl *expected, LmbHostControl *out) {
    uint8_t key[32], root[32], actual_root[32];
    if (!lmb_secure_enabled() || !key_text || !root_text || strlen(key_text)!=64 || strlen(root_text)!=64 ||
        lmb_unhex(key,key_text,32) || lmb_unhex(root,root_text,32) ||
        action>LMB_HOST_CONTROL_RETIRE || (action && !expected)) return -1;
    uint64_t now=lmb_io_monotonic_ms(), prior=lmb_read_deadline_ms;
    if (!now) return -1;
    uint32_t timeout=action==LMB_HOST_CONTROL_RETIRE ? 30000*LMB_HOST_MAX_SESSIONS+2000 : 6000;
    lmb_read_deadline_ms=prior && prior<now+timeout ? prior : now+timeout;
    int fd=lmb_connect_ms_io(address,2000,(int)timeout);
    if (fd<0) { lmb_read_deadline_ms=prior; return -1; }
    LmbBuf body={0}; LmbMsg reply={0}; LmbHostControl state={0}; uint32_t version=0,status=0;
    int bad=!lmb_secure_peer_matches(fd,key) || lmb_auth(fd) ||
        lmb_host_control_request(&body,action,root,expected) ||
        lmb_send(fd,LMB_HOST_CONTROL,body.p,(uint32_t)body.len,NULL,0) ||
        lmb_recv_bounded(fd,&reply,LMB_HOST_CONTROL_REPLY_BYTES,(int)timeout) || reply.op!=LMB_HOST_CONTROL_R || reply.pay_len;
    LmbCur c={reply.body,reply.body_len,0};
    if (!bad) bad=lmb_cur_u32(&c,&version) || version!=LMB_HOST_CONTROL_VERSION ||
        lmb_cur_u32(&c,&status) || status>LMB_HOST_CONTROL_CONFLICT ||
        lmb_cur_bytes(&c,actual_root,32) || memcmp(actual_root,root,32) ||
        lmb_cur_bytes(&c,state.instance,32) || lmb_cur_u64(&c,&state.revision) || !state.revision ||
        lmb_cur_u32(&c,&state.draining) || state.draining>LMB_HOST_CONTROL_RETIRED ||
        lmb_cur_u32(&c,&state.connections) || state.connections>LMB_HOST_MAX_SESSIONS ||
        lmb_cur_u32(&c,&state.requests) || state.requests>state.connections || c.off!=c.len;
    if (!bad && state.draining==LMB_HOST_CONTROL_RETIRED && (state.connections || state.requests)) bad=1;
    uint8_t nonzero=0;
    for (size_t i=0;i<sizeof state.instance;i++) nonzero|=state.instance[i];
    if (!nonzero) bad=1;
    if (!bad && !status && action) bad=memcmp(state.instance,expected->instance,32) ||
        expected->revision==UINT64_MAX || state.revision!=expected->revision+1 ||
        state.draining!=(action==LMB_HOST_CONTROL_RETIRE ? LMB_HOST_CONTROL_RETIRED : action==LMB_HOST_CONTROL_DRAIN);
    free(body.p); lmb_msg_free(&reply); lmb_close(fd);
    lmb_read_deadline_ms=prior;
    if (bad) return -1;
    *out=state; return (int)status;
}
#endif
