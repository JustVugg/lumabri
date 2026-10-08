#define _POSIX_C_SOURCE 200809L
#include "lumabri_proto.h"
#include "src/runtime/lumabri_node_control.h"
#include <assert.h>
#include <sys/wait.h>

int main(void) {
    signal(SIGPIPE,SIG_IGN);
    LmbNodeControl state={.instance={1},.revision=1,.sessions=2,.experts=3};
    uint8_t request[LMB_NODE_CONTROL_REQUEST_BYTES],reply[LMB_NODE_CONTROL_REPLY_BYTES],wrong[32]={2};
    assert(lmb_node_control_apply(&state,LMB_NODE_CONTROL_DRAIN,wrong,1));
    assert(!lmb_node_control_apply(&state,LMB_NODE_CONTROL_DRAIN,state.instance,1));
    assert(state.draining && state.revision==2 && state.sessions==2 && state.experts==3);
    assert(!lmb_node_control_apply(&state,LMB_NODE_CONTROL_DRAIN,state.instance,1));
    assert(lmb_node_control_apply(&state,LMB_NODE_CONTROL_RESUME,state.instance,1));
    assert(!lmb_node_control_apply(&state,LMB_NODE_CONTROL_RESUME,state.instance,2));
    assert(!state.draining && state.revision==3);
    assert(lmb_node_control_apply(&state,LMB_NODE_CONTROL_DRAIN,state.instance,1));
    lmb_node_control_request(request,LMB_NODE_CONTROL_DRAIN,&state);
    assert(lmb_get32(request)==1 && lmb_get32(request+4)==1 && lmb_node_control_u64(request+40)==3);
    lmb_node_control_reply(reply,0,&state);
    LmbNodeControl parsed;
    assert(!lmb_node_control_decode(reply,sizeof reply,&parsed) && parsed.sessions==2 && parsed.experts==3);
    assert(lmb_node_control_decode(reply,sizeof reply-1,&parsed)<0);
    lmb_put32(reply+48,2); assert(lmb_node_control_decode(reply,sizeof reply,&parsed)<0);
    memset(reply,0,sizeof reply); lmb_put32(reply,1); lmb_put32(reply+4,LMB_NODE_CONTROL_BUSY);
    assert(lmb_node_control_decode(reply,sizeof reply,&parsed)==LMB_NODE_CONTROL_BUSY && !parsed.revision);
    reply[8]=1; assert(lmb_node_control_decode(reply,sizeof reply,&parsed)<0);
    state.revision=UINT64_MAX;
    assert(lmb_node_control_apply(&state,LMB_NODE_CONTROL_DRAIN,state.instance,UINT64_MAX));
    lmb_node_control_reply(reply,1,&state);
    assert(lmb_node_control_decode(reply,sizeof reply,&parsed)==1 && parsed.revision==UINT64_MAX);
    /* Fragmented reply over the actual inherited-channel protocol. */
    int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        close(pair[0]); uint8_t received[sizeof request];
        uint64_t deadline=lmb_io_monotonic_ms()+2000;
        assert(!lmb_node_control_io(pair[1],received,sizeof received,0,deadline));
        assert(!memcmp(received,request,sizeof request));
        for (size_t i=0;i<sizeof reply;i++) assert(!lmb_node_control_io(pair[1],reply+i,1,1,deadline));
        close(pair[1]); _exit(0);
    }
    close(pair[1]); assert(!lmb_node_control_local(&pair[0],request,reply));
    assert(lmb_node_control_decode(reply,sizeof reply,&parsed)==1);
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(lmb_node_control_local(&pair[0],request,reply)<0 && pair[0]==-1);
    /* An incomplete reply cannot become the next command's acknowledgement. */
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    double before=(double)lmb_io_monotonic_ms();
    assert(lmb_node_control_local(&pair[0],request,reply)<0 && pair[0]==-1);
    double elapsed=(double)lmb_io_monotonic_ms()-before;
    assert(elapsed>=1900 && elapsed<4000); close(pair[1]);
    puts("NODE CONTROL: PASS (fenced state, no conversation mutation, codec, fragments, failed channel is unknown)");
    return 0;
}
