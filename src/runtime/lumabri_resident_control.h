/* Common authenticated control used by both the TUI and inference API.
 * Saved plans are hints, never authority to recreate an allocation. */
#ifndef LUMABRI_RESIDENT_CONTROL_H
#define LUMABRI_RESIDENT_CONTROL_H
#include "lumabri_node_control.h"
static int home_resident_node_control(const LmbResidentPlan *p, uint32_t i, uint32_t action,
                                      const LmbNodeControl *expected, LmbNodeControl *out) {
    if (!home_resident_plan_valid(p) || !lmb_home_nonzero(p->allocation,32) ||
        i>=p->execution.count || !lmb_home_nonzero(p->peer_keys[i],32) ||
        action>LMB_NODE_CONTROL_RETIRE || (action && !expected)) return -1;
    uint64_t now=lmb_io_monotonic_ms(),prior=lmb_read_deadline_ms;
    if (!now) return -1;
    lmb_read_deadline_ms=prior && prior<now+6000 ? prior : now+6000;
    int fd=lmb_connect_ms_io(p->execution.nodes[i].address,1000,3000);
    if (fd<0) { lmb_read_deadline_ms=prior; return -1; }
    uint8_t body[64+LMB_NODE_CONTROL_REQUEST_BYTES]; memcpy(body,p->allocation,32);
    lmb_node_control_request(body+64,action,expected);
    LmbMsg msg={0}; LmbNodeControl decoded={0}; int status=-1;
    int bad=lmb_unhex(body+32,p->root,32) || !lmb_secure_peer_matches(fd,p->peer_keys[i]) || lmb_auth(fd) ||
        lmb_send(fd,LMB_HOME_NODE_CONTROL,body,sizeof body,NULL,0) ||
        lmb_recv_bounded(fd,&msg,64+LMB_NODE_CONTROL_REPLY_BYTES,3000) ||
        msg.op!=LMB_HOME_NODE_CONTROL_R || msg.pay_len || msg.body_len!=64+LMB_NODE_CONTROL_REPLY_BYTES ||
        memcmp(body,msg.body,64);
    if (!bad) status=lmb_node_control_decode(msg.body+64,msg.body_len-64,&decoded);
    if (!bad && !status && action) bad=memcmp(decoded.instance,expected->instance,32) ||
        expected->revision==UINT64_MAX || decoded.revision!=expected->revision+1 ||
        decoded.draining!=(action==LMB_NODE_CONTROL_RETIRE ? LMB_NODE_CONTROL_RETIRED : action==LMB_NODE_CONTROL_DRAIN);
    lmb_msg_free(&msg); lmb_close(fd); lmb_read_deadline_ms=prior;
    if (bad || status<0) return -1;
    *out=decoded; return status;
}
static int home_resident_peer(const LmbResidentPlan *p, uint32_t i, int release) {
    if (!home_resident_plan_valid(p) || !lmb_home_nonzero(p->allocation, 32) ||
        i >= p->execution.count || !lmb_home_nonzero(p->peer_keys[i], 32)) return -1;
    int fd = lmb_connect_ms_io(p->execution.nodes[i].address, 600, 800);
    if (fd < 0) return -1;
    uint8_t body[64]; memcpy(body, p->allocation, 32);
    int bad = lmb_unhex(body + 32, p->root, 32) || !lmb_secure_peer_matches(fd, p->peer_keys[i]) || lmb_auth(fd);
    LmbMsg msg = {0};
    if (!bad) bad = lmb_send(fd, release ? LMB_HOME_RELEASE : LMB_HOME_QUERY, body, sizeof body, NULL, 0) || lmb_recv(fd, &msg);
    lmb_close(fd);
    LmbCur c = {msg.body, msg.body_len, 0}; uint32_t version, phase, segment, host;
    char reason[160];
    if (!bad) bad = msg.op != LMB_HOME_STATUS || msg.pay_len || lmb_cur_u32(&c, &version) ||
        version != LMB_HOME_VERSION || c.len - c.off < 32;
    if (!bad) { bad = memcmp(c.p + c.off, p->allocation, 32); c.off += 32; }
    if (!bad) bad = lmb_cur_u32(&c, &phase) || lmb_cur_u32(&c, &segment) ||
        lmb_cur_u32(&c, &host) || lmb_inventory_string(&c, reason, sizeof reason) || c.off != c.len;
    if (!bad) bad = release ? phase != LMB_HOME_CLOSED || segment || host :
        !segment || (p->execution.nodes[i].edge ? phase != LMB_HOME_READY || !host : phase != LMB_HOME_SEGMENT_READY);
    lmb_msg_free(&msg); return bad ? -1 : 0;
}
static int home_resident_retired_release(const LmbResidentPlan *p, uint32_t i, const LmbNodeControl *expected) {
    if (!home_resident_plan_valid(p) || i>=p->execution.count || !expected ||
        expected->draining!=LMB_NODE_CONTROL_RETIRED || !expected->revision || expected->sessions || expected->experts) return -1;
    uint64_t now=lmb_io_monotonic_ms(),prior=lmb_read_deadline_ms;
    if (!now) return -1;
    lmb_read_deadline_ms=prior && prior<now+10000 ? prior : now+10000;
    int fd=lmb_connect_ms_io(p->execution.nodes[i].address,1000,8000);
    if (fd<0) { lmb_read_deadline_ms=prior; return -1; }
    uint8_t body[64+LMB_NODE_CONTROL_REQUEST_BYTES]; memcpy(body,p->allocation,32);
    lmb_node_control_request(body+64,LMB_NODE_CONTROL_QUERY,expected);
    LmbMsg msg={0}; LmbNodeControl actual={0};
    int bad=lmb_unhex(body+32,p->root,32) || !lmb_secure_peer_matches(fd,p->peer_keys[i]) || lmb_auth(fd) ||
        lmb_send(fd,LMB_HOME_RETIRED_RELEASE,body,sizeof body,NULL,0) ||
        lmb_recv_bounded(fd,&msg,64+LMB_NODE_CONTROL_REPLY_BYTES,8000) || msg.pay_len ||
        msg.op!=LMB_HOME_RETIRED_RELEASE_R || msg.body_len!=64+LMB_NODE_CONTROL_REPLY_BYTES || memcmp(body,msg.body,64) ||
        lmb_node_control_decode(msg.body+64,LMB_NODE_CONTROL_REPLY_BYTES,&actual) ||
        actual.draining!=LMB_NODE_CONTROL_RETIRED || actual.revision!=expected->revision ||
        memcmp(actual.instance,expected->instance,32) || actual.sessions || actual.experts;
    lmb_msg_free(&msg); lmb_close(fd); lmb_read_deadline_ms=prior;
    return bad ? -1 : 0;
}
#endif
