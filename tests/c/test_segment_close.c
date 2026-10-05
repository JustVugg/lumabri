/* Exercise the actual Segment retirement path over fresh encrypted sockets.
 * A dead idle connection or BUSY response must not leak a conversation slot. */
#define main lmb_segment_cli_main
#include "../../segment_chat.c"
#undef main
#include <assert.h>

typedef struct { int listener, mode; unsigned requests; } ClosePeer;
static void *close_peer(void *arg) {
    ClosePeer *peer = arg;
    unsigned wanted = peer->mode == 1 || peer->mode == 2 ? 2 : peer->mode == 3 ? 4 : 1;
    LmbSegControl first = {0};
    for (unsigned i = 0; i < wanted; i++) {
        struct pollfd p = {peer->listener, POLLIN, 0}; assert(poll(&p,1,4000) == 1);
        int fd = accept(peer->listener,NULL,NULL); assert(fd >= 0);
        assert(!lmb_secure_server(fd));
        LmbMsg request = {0}; LmbSegControl control;
        assert(!lmb_recv_bounded(fd,&request,4096,2000) && request.op == LMB_SEG_CLOSE && !request.pay_len);
        assert(!lmb_seg_control_decode(request.body,request.body_len,&control));
        lmb_msg_free(&request);
        if (!i) first = control;
        else assert(lmb_seg_id_equal(&first.session_id,&control.session_id) &&
                    lmb_seg_id_equal(&first.request_id,&control.request_id) && first.sequence == control.sequence);
        peer->requests++;
        if (peer->mode == 2 && !i) { lmb_close(fd); continue; } /* lost reply */
        LmbSegReply reply = {.session_id=control.session_id,.request_id=control.request_id,
            .route_generation=control.owner.route_generation,
            .status=peer->mode == 3 || (peer->mode == 1 && !i) ? LMB_SEG_STATUS_BUSY :
                    peer->mode == 4 ? LMB_SEG_STATUS_NOT_FOUND : LMB_SEG_STATUS_OK};
        if (peer->mode == 5) reply.request_id.bytes[0] ^= 1;
        uint8_t *body = NULL; uint32_t size = 0;
        assert(!lmb_seg_reply_encode(&reply,&body,&size));
        assert(!lmb_send(fd,LMB_SEG_CLOSE_R,body,size,NULL,0)); free(body); lmb_close(fd);
    }
    return NULL;
}
static void check(unsigned mode) {
    ClosePeer peer = {.listener=lmb_listen(0),.mode=(int)mode}; assert(peer.listener >= 0);
    struct sockaddr_in address; socklen_t length=sizeof address;
    assert(!getsockname(peer.listener,(struct sockaddr *)&address,&length));
    SegmentConversation conversation = {.active=1,.chain_count=1};
    RemoteSegment *remote=&conversation.chain[0];
    remote->opened=1; remote->route.transport=LMB_SEG_TRANSPORT_DIRECT;
    snprintf(remote->route.advert.addr,sizeof remote->route.advert.addr,"127.0.0.1:%u",ntohs(address.sin_port));
    strcpy(remote->route.advert.peer_name,"retained-test");
    remote->open.owner.route_generation=7;
    remote->open.owner.lease_id.bytes[0]=1; remote->open.owner.fencing_epoch=1;
    remote->open.session_id.bytes[0]=2; remote->sequence=3;
    int idle[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,idle)); close(idle[1]);
    remote->fd=idle[0];
    pthread_t server; assert(!pthread_create(&server,NULL,close_peer,&peer));
    int rc=conversation_reset(&conversation);
    pthread_join(server,NULL); close(peer.listener);
    if (mode == 3 || mode == 5) {
        assert(rc && !conversation.active && conversation.chain_count==1 && conversation.chain[0].opened);
        assert(conversation.chain[0].fd<0); /* identities retained; no silent RESET_DONE */
    } else assert(!rc && !conversation.active && !conversation.chain_count);
    assert(peer.requests == (mode == 1 || mode == 2 ? 2u : mode == 3 ? 4u : 1u));
}
int main(void) {
    char temporary[]="/tmp/lumabri-close-XXXXXX"; assert(mkdtemp(temporary));
    char key[160], known[160]; snprintf(key,sizeof key,"%s/key",temporary); snprintf(known,sizeof known,"%s/known",temporary);
    assert(!setenv("LUMABRI_PEER_KEY",key,1) && !setenv("LUMABRI_KNOWN_HOSTS",known,1) && !setenv("LUMABRI_ENCRYPT","1",1));
    signal(SIGPIPE,SIG_IGN); assert(!lmb_secure_init()); segment_direct_only=1;
    for (unsigned mode=0; mode<6; mode++) check(mode);
    assert(!unlink(key)); (void)unlink(known); assert(!rmdir(temporary));
    puts("SEGMENT CLOSE: PASS (idle reconnect, acknowledged release, BUSY/lost-reply retry, absent session, mismatch, failed reset retained)");
    return 0;
}
