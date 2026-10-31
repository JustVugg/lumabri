/* Real native Segment process supervisor for the cross-platform network gate.
 * The caller supplies a reduced checkpoint and a separate test tracker. */
#include "lumabri_home_net.h"
#include "src/runtime/lumabri_node_control.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include <assert.h>
#include "segment_worker_fixture_windows.h"
#include "segment_chat_fixture_windows.h"
#include "host_gateway_fixture_windows.h"

int wmain(int argc,wchar_t **wide) {
    assert(argc==4 || argc==5); /* worker.exe, checkpoint, tracker:port, optional chat.exe */
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    char binary[6144],model[6144],tracker[128];
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[1],-1,binary,sizeof binary,NULL,NULL));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[2],-1,model,sizeof model,NULL,NULL));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[3],-1,tracker,sizeof tracker,NULL,NULL));
    int remote=!strcmp(tracker,"remote-selftest");
    int selftest=remote || !strcmp(tracker,"selftest");
    wchar_t temp[2048],folder[2304];
    assert(GetTempPathW(2048,temp));
    assert(swprintf(folder,2304,L"%lslumabri worker \x00e8\x03a9-%lu-%llu",temp,
        (unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64())>0);
    assert(CreateDirectoryW(folder,NULL));
    char directory[6144],key[6400],known[6400];
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,folder,-1,directory,sizeof directory,NULL,NULL));
    assert(snprintf(key,sizeof key,"LUMABRI_PEER_KEY=%s\\worker.key",directory)>0);
    assert(snprintf(known,sizeof known,"LUMABRI_KNOWN_HOSTS=%s\\known",directory)>0);
    const char *environment[]={key,known,"LUMABRI_TOKEN=native-worker-test","LUMABRI_ENCRYPT=1",
        "LUMABRI_RESIDENT_REQUIRED=1","LUMABRI_RESIDENCY_POLICY=engine-locked",
        "LUMABRI_SEGMENT_RAM_RESERVE_MB=256","LUMABRI_ENGINE_BACKEND=cpu",
        "LUMABRI_COMPUTE_SOCKET=","PIN=off","PILOT=0","HOT=0","COLI_NO_OMP_TUNE=1"};
    WorkerFixture fixture;
    if (selftest) {
        char parent_key[6400],parent_known[6400];
        snprintf(parent_key,sizeof parent_key,"%s\\parent.key",directory);
        snprintf(parent_known,sizeof parent_known,"%s\\parent.known",directory);
        assert(!_putenv_s("LUMABRI_PEER_KEY",parent_key) && !_putenv_s("LUMABRI_KNOWN_HOSTS",parent_known));
        assert(!_putenv_s("LUMABRI_ENCRYPT","1") && !_putenv_s("LUMABRI_TOKEN","native-worker-test"));
        assert(!lmb_secure_init() && lmb_secure_enabled());
        assert(!coli_olmoe_edge_adapter_register() && !coli_olmoe_segment_adapter_register());
        fixture_start(&fixture,tracker,sizeof tracker,remote ? model : NULL);
    }
    LmbSocket listener=lmb_listen(0);
    assert(listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in bound; int bound_size=sizeof bound;
    assert(!getsockname(listener,(struct sockaddr *)&bound,&bound_size));
    int port=ntohs(bound.sin_port); assert(port>0);
    HANDLE listener_pipe[2],preflight[2],control[2];
    assert(!lmb_pipe_pair(listener_pipe) && !lmb_pipe_pair(preflight) && !lmb_pipe_pair(control));
    HANDLE ready=CreateEventW(NULL,TRUE,FALSE,NULL); assert(ready);
    LmbChildHandle handles[]={{"LUMABRI_HOME_LISTEN_HANDLE",listener_pipe[1]},
        {"LUMABRI_PREFLIGHT_HANDLE",preflight[1]},
        {"LUMABRI_NODE_CONTROL_HANDLE",control[1]}, {"LUMABRI_READY_HANDLE",ready}};
    char port_text[16],address[64];
    snprintf(port_text,sizeof port_text,"%d",port);snprintf(address,sizeof address,"127.0.0.1:%d",port);
    char placeholder[6400],signer[65],root[65],size_text[32];
    if (remote) {
        snprintf(placeholder,sizeof placeholder,"%s\\checkpoint",directory);
        lmb_hex(signer,fixture.checkpoint.key,32);lmb_hex(root,fixture.checkpoint.root,32);
        snprintf(size_text,sizeof size_text,"%llu",(unsigned long long)fixture.checkpoint.bytes);
    }
    const char *args[]={"--engine","olmoe","--model-dir",remote ? placeholder : model,"--model","native-worker-olmoe",
        "--range","0:4","--port",port_text,"--tracker",tracker,"--advertise",address,
        "--name","windows-worker","--model-root",remote ? root : "1111111111111111111111111111111111111111111111111111111111111111",
        "--tokenizer-root","2222222222222222222222222222222222222222222222222222222222222222",
        "--context",argc==5 ? "512" : "64","--max-rows","16","--sessions",argc==5 ? "2" : "1","--threads","1","--memory-limit-mb","128",
        "--checkpoint-signer",signer,"--model-bytes",size_text,"--model-layers","4"};
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=sizeof args/sizeof *args-(remote ? 0 : 6),
        .environment=environment,.nenvironment=sizeof environment/sizeof *environment,
        .handles=handles,.nhandles=sizeof handles/sizeof *handles,
        .output=GetStdHandle(STD_ERROR_HANDLE),.error=GetStdHandle(STD_ERROR_HANDLE)};
    LmbOwnedProcess child={0}; assert(!lmb_owned_process_start(&child,&launch));
    CloseHandle(listener_pipe[1]);CloseHandle(preflight[1]);CloseHandle(control[1]);
    assert(!lmb_home_export_listener(listener,GetProcessId(child.process),listener_pipe[0]));
    char stage=0; assert(!lmb_pipe_io(preflight[0],&stage,1,0,GetTickCount64()+15000) && stage=='P');
    CloseHandle(preflight[0]);
    HANDLE waiting[]={ready,child.process};
    DWORD status=WaitForMultipleObjects(2,waiting,FALSE,30000);
    if (status!=WAIT_OBJECT_0) {
        DWORD code=0; GetExitCodeProcess(child.process,&code);
        fprintf(stderr,"worker not READY: wait=%lu child=%lu\n",(unsigned long)status,(unsigned long)code);
        lmb_owned_process_stop(&child,0,3000);lmb_owned_process_close(&child);return 1;
    }
    lmb_socket_close(listener);CloseHandle(listener_pipe[0]);CloseHandle(ready);
    uint64_t preparation_reads=remote ? atomic_load(&fixture.checkpoint.reads) : 0;
    uint8_t request[LMB_NODE_CONTROL_REQUEST_BYTES],reply[LMB_NODE_CONTROL_REPLY_BYTES];
    LmbNodeControl current;
    lmb_node_control_request(request,LMB_NODE_CONTROL_QUERY,NULL);
    assert(!lmb_node_control_local(&control[0],request,reply));
    assert(!lmb_node_control_decode(reply,sizeof reply,&current));
    assert(current.revision==1 && !current.sessions && !current.draining);
    printf("READY %d %lu\n",port,(unsigned long)GetProcessId(child.process));fflush(stdout);
    if (selftest) {fixture_chat(&fixture,model,0);fixture_chat(&fixture,model,0);}
    if (argc==5) {
        char chat_binary[6144];assert(remote);
        assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[4],-1,chat_binary,sizeof chat_binary,NULL,NULL));
        fixture_native_chat(&fixture,chat_binary,directory,tracker);
        fixture_native_host(&fixture,chat_binary,directory,tracker);
        preparation_reads=atomic_load(&fixture.checkpoint.reads);
    }
    char command[32];
    const char *checks[]={"query\n","drain\n","resume\n","drain\n","retire\n","stop\n"};
    for (unsigned step=0;;step++) {
        if (selftest) {assert(step<sizeof checks/sizeof *checks);strcpy(command,checks[step]);}
        else if (!fgets(command,sizeof command,stdin)) break;
        if (!strcmp(command,"stop\n")) break;
        uint32_t action=!strcmp(command,"query\n") ? LMB_NODE_CONTROL_QUERY :
            !strcmp(command,"drain\n") ? LMB_NODE_CONTROL_DRAIN :
            !strcmp(command,"resume\n") ? LMB_NODE_CONTROL_RESUME :
            !strcmp(command,"retire\n") ? LMB_NODE_CONTROL_RETIRE : UINT32_MAX;
        assert(action!=UINT32_MAX);
        lmb_node_control_request(request,action,action ? &current : NULL);
        assert(!lmb_node_control_local(&control[0],request,reply));
        int decoded=lmb_node_control_decode(reply,sizeof reply,&current);assert(decoded>=0);
        if (selftest) {
            assert(!decoded && !current.sessions && !current.experts);
            if (action==LMB_NODE_CONTROL_DRAIN || action==LMB_NODE_CONTROL_RETIRE) fixture_chat(&fixture,model,1);
            else if (action==LMB_NODE_CONTROL_RESUME) fixture_chat(&fixture,model,0);
        }
        printf("CONTROL %d %llu %u %u %u\n",decoded,(unsigned long long)current.revision,
            current.draining,current.sessions,current.experts);fflush(stdout);
    }
    assert(!lmb_owned_process_stop(&child,0,5000));
    assert(!lmb_owned_process_close(&child));CloseHandle(control[0]);
    if (selftest) {
        atomic_store(&fixture.stop,1);pthread_join(fixture.thread,NULL);
        lmb_close(fixture.listener);CloseHandle(fixture.registered);pthread_mutex_destroy(&fixture.lock);
        if (remote) {
            assert(preparation_reads && atomic_load(&fixture.checkpoint.reads)==preparation_reads);
            for (uint32_t i=0;i<fixture.checkpoint.count;i++) {
                char file[8192];wchar_t wide_file[8192];
                assert(snprintf(file,sizeof file,"%s/%s",placeholder,fixture.checkpoint.items[i].path)<(int)sizeof file);
                assert(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,file,-1,wide_file,8192));
                (void)DeleteFileW(wide_file);
            }
            wchar_t wide_folder[8192];assert(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,placeholder,-1,wide_folder,8192));
            (void)RemoveDirectoryW(wide_folder);checkpoint_fixture_free(&fixture.checkpoint);
        }
    }
    wchar_t path[2400];
    swprintf(path,2400,L"%ls\\worker.key",folder);DeleteFileW(path);
    swprintf(path,2400,L"%ls\\known",folder);DeleteFileW(path);
    swprintf(path,2400,L"%ls\\parent.key",folder);DeleteFileW(path);
    swprintf(path,2400,L"%ls\\parent.known",folder);DeleteFileW(path);
    if (argc==5) {
        swprintf(path,2400,L"%ls\\edge.key",folder);DeleteFileW(path);
        swprintf(path,2400,L"%ls\\edge.known",folder);DeleteFileW(path);
        swprintf(path,2400,L"%ls\\edge-oracle",folder);RemoveDirectoryW(path);
        swprintf(path,2400,L"%ls\\edge-serve",folder);RemoveDirectoryW(path);
    }
    RemoveDirectoryW(folder);
    puts("WORKER STOPPED");
    if (selftest) puts("NATIVE WORKER: PASS (72 oracle tokens, encrypted production process, isolated turns, drain refusal/resume/retire)");
    return 0;
}
