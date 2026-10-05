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
typedef struct { int listener; unsigned mode, requests, refreshes; LmbSegRouteEntry route; } OpenPeer;
static void *open_peer(void *arg) {
    OpenPeer *peer=arg;
    unsigned wanted=peer->mode==0 ? 2 : peer->mode==1 ? 3 : 1;
    LmbSegId session={0};
    for (unsigned i=0;i<wanted;i++) {
        struct pollfd p={peer->listener,POLLIN,0}; assert(poll(&p,1,4000)==1);
        int fd=accept(peer->listener,NULL,NULL); assert(fd>=0 && !lmb_secure_server(fd));
        LmbMsg request={0}; LmbSegOpen open;
        assert(!lmb_recv_bounded(fd,&request,4096,2000) && request.op==LMB_SEG_OPEN && !request.pay_len);
        assert(!lmb_seg_open_decode(request.body,request.body_len,&open)); lmb_msg_free(&request);
        if (!i) session=open.session_id; else assert(lmb_seg_id_equal(&session,&open.session_id));
        assert(open.owner.route_generation==7+i);
        peer->requests++;
        LmbSegReply reply={.session_id=open.session_id,.request_id=open.request_id,
            .route_generation=open.owner.route_generation,.fencing_epoch=open.owner.fencing_epoch,
            .status=peer->mode==0 && i ? LMB_SEG_STATUS_OK : peer->mode==4 ? LMB_SEG_STATUS_QUOTA : LMB_SEG_STATUS_STALE_OWNER};
        if (peer->mode==5) reply.request_id.bytes[0]^=1;
        uint8_t *body=NULL; uint32_t size=0;
        assert(!lmb_seg_reply_encode(&reply,&body,&size));
        assert(!lmb_send(fd,LMB_SEG_OPEN_R,body,size,NULL,0)); free(body); lmb_close(fd);
    }
    return NULL;
}
static int open_refresh(void *arg, LmbSegRouteSnapshot *snapshot) {
    OpenPeer *peer=arg; peer->refreshes++;
    snapshot->complete=1; snapshot->count=1;
    snapshot->entries[0]=peer->route;
    snapshot->entries[0].owner.route_generation+=peer->refreshes;
    if (peer->mode==2) snapshot->entries[0].owner.lease_id.bytes[0]^=1;
    if (peer->mode==3) snapshot->entries[0].advert.addr[0]^=1;
    return 1;
}
static void check_open(unsigned mode) {
    OpenPeer peer={.listener=lmb_listen(0),.mode=mode}; assert(peer.listener>=0);
    struct sockaddr_in address; socklen_t length=sizeof address;
    assert(!getsockname(peer.listener,(struct sockaddr *)&address,&length));
    RemoteSegment remote={.fd=-1}; remote.route.transport=LMB_SEG_TRANSPORT_DIRECT;
    LmbSegAdvert *a=&remote.route.advert;
    snprintf(a->addr,sizeof a->addr,"127.0.0.1:%u",ntohs(address.sin_port));
    strcpy(a->peer_name,"open-test"); strcpy(a->model,"fixture");
    a->model_root[0]=1; a->tokenizer_root[0]=2; a->layer_end=1;
    a->max_context=64; a->max_rows=1; a->state_dtype=LMB_SEG_DTYPE_F32; a->state_width=8;
    a->max_sessions=1; a->capabilities=LMB_SEG_CAP_CPU|LMB_SEG_CAP_RANGE_NATIVE|LMB_SEG_CAP_TOKEN_IDS|LMB_SEG_CAP_MULTI_SESSION;
    strcpy(a->engine_id,"olmoe"); strcpy(a->state_schema,"kv-standard-v1"); strcpy(a->numeric_class,"strict-tiny");
    remote.route.owner.lease_id.bytes[0]=3;
    remote.route.owner.fencing_epoch=1; remote.route.owner.route_generation=7;
    peer.route=remote.route;
    LmbSegId session={{4}}; char why[320]; uint8_t root[32]={1}, tokenizer[32]={2};
    pthread_t server; assert(!pthread_create(&server,NULL,open_peer,&peer));
    int rc=remote_open_current(&remote,&session,root,tokenizer,64,1,open_refresh,&peer,why,sizeof why);
    if (rc) fprintf(stderr,"OPEN fixture mode %u: %s\n",mode,why);
    pthread_join(server,NULL); close(peer.listener);
    assert(mode ? rc && !remote.opened : !rc && remote.opened);
    assert(peer.requests==(mode==0 ? 2u : mode==1 ? 3u : 1u));
    assert(peer.refreshes==(mode==0 || mode==2 || mode==3 ? 1u : mode==1 ? 2u : 0u));
    assert(remote.fd<0);
    /* Every immutable allocation field must remain fenced during refresh. */
    LmbSegRouteEntry changed=peer.route;
    changed.owner.fencing_epoch++; assert(!open_route_refreshable(&peer.route,&changed));
    changed=peer.route; changed.advert.model_root[0]^=1; assert(!open_route_refreshable(&peer.route,&changed));
    changed=peer.route; changed.advert.numeric_class[0]^=1; assert(!open_route_refreshable(&peer.route,&changed));
}
int main(void) {
    char temporary[]="/tmp/lumabri-close-XXXXXX"; assert(mkdtemp(temporary));
    char key[160], known[160]; snprintf(key,sizeof key,"%s/key",temporary); snprintf(known,sizeof known,"%s/known",temporary);
    assert(!setenv("LUMABRI_PEER_KEY",key,1) && !setenv("LUMABRI_KNOWN_HOSTS",known,1) && !setenv("LUMABRI_ENCRYPT","1",1));
    signal(SIGPIPE,SIG_IGN); assert(!lmb_secure_init()); segment_direct_only=1;
    for (unsigned mode=0; mode<6; mode++) check(mode);
    for (unsigned mode=0; mode<6; mode++) check_open(mode);
    assert(!unlink(key)); (void)unlink(known); assert(!rmdir(temporary));
    puts("SEGMENT CLOSE/OPEN: PASS (acknowledged retirement, bounded same-lease OPEN refresh, changed owner/endpoint refusal, quota/mismatch not retried)");
    return 0;
}
