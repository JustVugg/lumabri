/* Private keeper -> Segment control. Possession of the inherited socket is
 * the local capability; no extra identity is admitted to the Segment data
 * listener. The keeper authenticates the remote allocation owner separately.
 * All counts and mutations are serialized with OPEN/Hybrid admission. */
#ifndef LMB_NODE_CONTROL_H
#define LMB_NODE_CONTROL_H
#define LMB_NODE_CONTROL_VERSION 1u
#define LMB_NODE_CONTROL_QUERY 0u
#define LMB_NODE_CONTROL_DRAIN 1u
#define LMB_NODE_CONTROL_RESUME 2u
#define LMB_NODE_CONTROL_CONFLICT 1u
#define LMB_NODE_CONTROL_BUSY 2u
#define LMB_NODE_CONTROL_REQUEST_BYTES 48u
#define LMB_NODE_CONTROL_REPLY_BYTES 60u
typedef struct {
    uint8_t instance[32];
    uint64_t revision;
    uint32_t draining, sessions, experts;
} LmbNodeControl;
static LMB_MAYBE_UNUSED uint64_t lmb_node_control_u64(const uint8_t *p) {
    return (uint64_t)lmb_get32(p) | ((uint64_t)lmb_get32(p+4)<<32);
}
static LMB_MAYBE_UNUSED void lmb_node_control_put64(uint8_t *p, uint64_t value) {
    lmb_put32(p,(uint32_t)value); lmb_put32(p+4,(uint32_t)(value>>32));
}

static LMB_MAYBE_UNUSED int lmb_node_control_apply(LmbNodeControl *s, uint32_t action,
                                                  const uint8_t instance[32], uint64_t revision) {
    if (action==LMB_NODE_CONTROL_QUERY) return 0;
    if (action>LMB_NODE_CONTROL_RESUME || !revision || memcmp(instance,s->instance,32)) return -1;
    uint32_t draining=action==LMB_NODE_CONTROL_DRAIN;
    if (s->revision>1 && revision==s->revision-1 && s->draining==draining) return 0;
    if (revision!=s->revision || s->revision==UINT64_MAX) return -1;
    s->draining=draining; s->revision++; return 0;
}
static LMB_MAYBE_UNUSED void lmb_node_control_request(uint8_t out[LMB_NODE_CONTROL_REQUEST_BYTES],
                                                      uint32_t action, const LmbNodeControl *expected) {
    memset(out,0,LMB_NODE_CONTROL_REQUEST_BYTES);
    lmb_put32(out,LMB_NODE_CONTROL_VERSION); lmb_put32(out+4,action);
    if (expected) { memcpy(out+8,expected->instance,32); lmb_node_control_put64(out+40,expected->revision); }
}
static LMB_MAYBE_UNUSED void lmb_node_control_reply(uint8_t out[LMB_NODE_CONTROL_REPLY_BYTES],
                                                    uint32_t status, const LmbNodeControl *s) {
    lmb_put32(out,LMB_NODE_CONTROL_VERSION); lmb_put32(out+4,status);
    memcpy(out+8,s->instance,32); lmb_node_control_put64(out+40,s->revision);
    lmb_put32(out+48,s->draining); lmb_put32(out+52,s->sessions); lmb_put32(out+56,s->experts);
}
static LMB_MAYBE_UNUSED int lmb_node_control_decode(const uint8_t *in, size_t bytes, LmbNodeControl *s) {
    if (bytes!=LMB_NODE_CONTROL_REPLY_BYTES || lmb_get32(in)!=LMB_NODE_CONTROL_VERSION ||
        lmb_get32(in+4)>LMB_NODE_CONTROL_BUSY) return -1;
    if (lmb_get32(in+4)==LMB_NODE_CONTROL_BUSY) {
        for (unsigned i=8;i<LMB_NODE_CONTROL_REPLY_BYTES;i++) if (in[i]) return -1;
        memset(s,0,sizeof *s); return LMB_NODE_CONTROL_BUSY; /* no idle snapshot */
    }
    memcpy(s->instance,in+8,32); s->revision=lmb_node_control_u64(in+40);
    s->draining=lmb_get32(in+48); s->sessions=lmb_get32(in+52); s->experts=lmb_get32(in+56);
    uint8_t nonzero=0; for (unsigned i=0;i<32;i++) nonzero|=s->instance[i];
    if (!nonzero || !s->revision || s->draining>1 || s->sessions>256 || s->experts>256) return -1;
    return (int)lmb_get32(in+4);
}
/* Raw AF_UNIX only, never an encrypted network descriptor. Absolute deadline
 * covers partial messages too; a timed-out RPC poisons the channel rather
 * than letting a late reply be mistaken for the following command. */
static LMB_MAYBE_UNUSED int lmb_node_control_io(int fd, void *data, size_t size,
                                               int writing, uint64_t deadline) {
    uint8_t *p=data;
    while (size) {
        uint64_t now=lmb_io_monotonic_ms();
        if (!now || now>=deadline) return -1;
        struct pollfd ready={fd,writing ? POLLOUT : POLLIN,0};
        int rc=poll(&ready,1,(int)(deadline-now));
        if (rc<0 && errno==EINTR) continue;
        if (rc<=0 || !(ready.revents & ready.events)) return -1;
        ssize_t n=writing ? send(fd,p,size,MSG_DONTWAIT|MSG_NOSIGNAL) : recv(fd,p,size,MSG_DONTWAIT);
        if (n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n<=0) return -1;
        p+=(size_t)n; size-=(size_t)n;
    }
    return 0;
}
static LMB_MAYBE_UNUSED int lmb_node_control_local(int *fd, const uint8_t *request, uint8_t *reply) {
    if (*fd<0) return -1;
    uint64_t now=lmb_io_monotonic_ms();
    LmbNodeControl decoded;
    int bad=!now || lmb_node_control_io(*fd,(void *)request,LMB_NODE_CONTROL_REQUEST_BYTES,1,now+2000) ||
        lmb_node_control_io(*fd,reply,LMB_NODE_CONTROL_REPLY_BYTES,0,now+2000) ||
        lmb_node_control_decode(reply,LMB_NODE_CONTROL_REPLY_BYTES,&decoded)<0;
    if (bad) { (close)(*fd); *fd=-1; return -1; }
    return 0;
}
#endif
