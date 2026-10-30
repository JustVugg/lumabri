/* Native inherited control uses the same frames and mutation fences as Unix. */
#include "lumabri_proto.h"
#include "src/runtime/lumabri_node_control.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include <assert.h>

static int worker(int partial) {
    LmbNodeChannel channel;
    assert(!lmb_pipe_inherit(L"LUMABRI_NODE_CONTROL_HANDLE",&channel));
    assert(channel!=LMB_NODE_CHANNEL_INVALID);
    DWORD flags=0; assert(GetHandleInformation(channel,&flags) && !(flags&HANDLE_FLAG_INHERIT));
    assert(!GetEnvironmentVariableW(L"LUMABRI_NODE_CONTROL_HANDLE",NULL,0));
    LmbNodeControl state={.instance={7},.revision=1,.sessions=1};
    for (;;) {
        int ready=lmb_node_channel_wait(channel,5000);
        if (ready<0) break;
        assert(ready==1);
        uint8_t request[LMB_NODE_CONTROL_REQUEST_BYTES],reply[LMB_NODE_CONTROL_REPLY_BYTES];
        uint64_t deadline=lmb_io_monotonic_ms()+2000;
        assert(!lmb_node_control_io(channel,request,sizeof request,0,deadline));
        assert(lmb_get32(request)==LMB_NODE_CONTROL_VERSION);
        int conflict=lmb_node_control_apply(&state,lmb_get32(request+4),request+8,lmb_node_control_u64(request+40));
        lmb_node_control_reply(reply,conflict ? LMB_NODE_CONTROL_CONFLICT : 0,&state);
        if (partial) {
            assert(!lmb_node_control_io(channel,reply,7,1,deadline));
            Sleep(INFINITE);
        }
        for (size_t i=0;i<sizeof reply;i++) assert(!lmb_node_control_io(channel,reply+i,1,1,deadline));
    }
    lmb_node_channel_close(channel); return 0;
}
static LmbOwnedProcess start(const char *binary,LmbNodeChannel *channel,int partial) {
    HANDLE pair[2]; assert(!lmb_pipe_pair(pair));
    LmbChildHandle inherited={"LUMABRI_NODE_CONTROL_HANDLE",pair[1]};
    const char *args[]={partial ? "--partial" : "--worker"};
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=1,.handles=&inherited,.nhandles=1,
        .error=GetStdHandle(STD_ERROR_HANDLE)};
    LmbOwnedProcess child={0}; assert(!lmb_owned_process_start(&child,&launch));
    CloseHandle(pair[1]); *channel=pair[0]; return child;
}
static unsigned exchange(LmbNodeChannel *channel,unsigned action,const LmbNodeControl *expected,LmbNodeControl *state) {
    uint8_t request[LMB_NODE_CONTROL_REQUEST_BYTES],reply[LMB_NODE_CONTROL_REPLY_BYTES];
    lmb_node_control_request(request,action,expected);
    assert(!lmb_node_control_local(channel,request,reply));
    int result=lmb_node_control_decode(reply,sizeof reply,state); assert(result>=0); return (unsigned)result;
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (argc==2 && !wcscmp(argv[1],L"--worker")) return worker(0);
    if (argc==2 && !wcscmp(argv[1],L"--partial")) return worker(1);
    assert(argc==1);
    wchar_t exe[2048]; char binary[6144];
    assert(GetModuleFileNameW(NULL,exe,2048));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,exe,-1,binary,sizeof binary,NULL,NULL));
    LmbNodeChannel channel;
    assert(SetEnvironmentVariableW(L"LUMABRI_NODE_CONTROL_HANDLE",L"1"));
    assert(lmb_pipe_inherit(L"LUMABRI_NODE_CONTROL_HANDLE",&channel) && channel==LMB_NODE_CHANNEL_INVALID);
    assert(!lmb_pipe_inherit(L"LUMABRI_NODE_CONTROL_HANDLE",&channel) && channel==LMB_NODE_CHANNEL_INVALID);
    LmbOwnedProcess child=start(binary,&channel,0);
    LmbNodeControl first,current;
    assert(!exchange(&channel,LMB_NODE_CONTROL_QUERY,NULL,&first));
    assert(first.instance[0]==7 && first.revision==1 && first.sessions==1);
    assert(!exchange(&channel,LMB_NODE_CONTROL_DRAIN,&first,&current));
    assert(current.draining && current.revision==2 && current.sessions==1);
    assert(!exchange(&channel,LMB_NODE_CONTROL_DRAIN,&first,&current));
    assert(exchange(&channel,LMB_NODE_CONTROL_RESUME,&first,&current)==LMB_NODE_CONTROL_CONFLICT);
    first=current;
    assert(exchange(&channel,LMB_NODE_CONTROL_RETIRE,&first,&current)==LMB_NODE_CONTROL_CONFLICT);
    assert(current.sessions==1 && current.revision==2);
    assert(!exchange(&channel,LMB_NODE_CONTROL_RESUME,&first,&current));
    assert(!current.draining && current.sessions==1 && current.revision==3);
    lmb_node_channel_close(channel);
    DWORD code; assert(!lmb_owned_process_wait(&child,5000,&code) && !code);
    assert(!lmb_owned_process_close(&child));
    child=start(binary,&channel,1);
    uint8_t request[LMB_NODE_CONTROL_REQUEST_BYTES],reply[LMB_NODE_CONTROL_REPLY_BYTES];
    lmb_node_control_request(request,LMB_NODE_CONTROL_QUERY,NULL);
    uint64_t before=lmb_io_monotonic_ms();
    assert(lmb_node_control_local(&channel,request,reply)==-1 && channel==LMB_NODE_CHANNEL_INVALID);
    uint64_t elapsed=lmb_io_monotonic_ms()-before; assert(elapsed>=1900 && elapsed<5000);
    assert(lmb_node_control_local(&channel,request,reply)==-1);
    assert(!lmb_owned_process_stop(&child,0,5000) && !lmb_owned_process_close(&child));
    child=start(binary,&channel,0);
    assert(!lmb_owned_process_stop(&child,0,5000) && !lmb_owned_process_close(&child));
    assert(lmb_node_control_local(&channel,request,reply)==-1 && channel==LMB_NODE_CHANNEL_INVALID);
    puts("NATIVE NODE CONTROL: PASS (inherited private capability, fragments, mutation fences, live session refusal, poisoned timeout, worker crash)");
    return 0;
}
