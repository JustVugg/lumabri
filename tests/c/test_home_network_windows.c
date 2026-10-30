#include "lumabri_home_net.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_ready.h"
#include <assert.h>

int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (argc==3 && !wcscmp(argv[1],L"--worker")) {
        int port=(int)wcstol(argv[2],NULL,10);
        LmbSocket listener=lmb_home_take_listener(port);
        if (listener==LMB_INVALID_SOCKET) return 64;
        assert(!lmb_ready_notify());
        assert(lmb_socket_wait(listener,0,5000)==1);
        LmbSocket client=lmb_socket_accept(listener,NULL,NULL); assert(client!=LMB_INVALID_SOCKET);
        assert(!lmb_socket_timeout(client,2000));
        char request[4]; assert(!lmb_socket_read_full(client,request,4) && !memcmp(request,"PING",4));
        assert(!lmb_write_full(client,"OK",2));
        lmb_socket_close(client); lmb_socket_close(listener); return 0;
    }
    assert(argc==1);
    assert(!lmb_socket_init());
    char address[INET_ADDRSTRLEN];
    assert(!_putenv_s("LUMABRI_ADVERTISE","100.101.102.103"));
    assert(!lmb_home_advertise_ip(address,sizeof address) && !strcmp(address,"100.101.102.103"));
    assert(!_putenv_s("LUMABRI_ADVERTISE","0.0.0.0"));
    assert(lmb_home_advertise_ip(address,sizeof address));
    assert(!_putenv_s("LUMABRI_ADVERTISE",""));
    if (!lmb_home_interface_ip(address,sizeof address)) {
        struct in_addr actual; assert(inet_pton(AF_INET,address,&actual)==1 && (ntohl(actual.s_addr)>>24)!=127);
    }
    LmbSocket sockets[LMB_HOME_PORT_COUNT]; int base=0;
    for (unsigned attempt=0;attempt<100 && !base;attempt++) {
        int candidate=30000+(int)((GetCurrentProcessId()+attempt)%1000)*16;
        unsigned n=0;
        for (;n<LMB_HOME_PORT_COUNT;n++) {
            sockets[n]=lmb_listen(candidate+(int)n);
            if (sockets[n]==LMB_INVALID_SOCKET) break;
        }
        for (unsigned i=0;i<n;i++) lmb_socket_close(sockets[i]);
        if (n==LMB_HOME_PORT_COUNT) base=candidate;
    }
    assert(base);
    char text[32]; snprintf(text,sizeof text,"%d",base);
    assert(!_putenv_s("LUMABRI_HOME_PORT_BASE",text));
    int port=0;
    for (unsigned i=0;i<LMB_HOME_PORT_COUNT-1;i++) {
        sockets[i]=lmb_home_listen_service(&port);
        assert(sockets[i]!=LMB_INVALID_SOCKET && port==base+(int)i+1);
        DWORD flags=0; assert(GetHandleInformation((HANDLE)sockets[i],&flags) && !(flags&HANDLE_FLAG_INHERIT));
    }
    assert(lmb_home_listen_service(&port)==LMB_INVALID_SOCKET);
    for (unsigned i=0;i<LMB_HOME_PORT_COUNT-1;i++) lmb_socket_close(sockets[i]);
    wchar_t exe[2048]; char binary[6144];
    assert(GetModuleFileNameW(NULL,exe,2048));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,exe,-1,binary,sizeof binary,NULL,NULL));
    for (unsigned wrong=0;wrong<2;wrong++) {
        LmbSocket listener=lmb_home_listen_service(&port); assert(listener!=LMB_INVALID_SOCKET);
        HANDLE pair[2]; assert(!lmb_pipe_pair(pair));
        HANDLE ready=CreateEventW(NULL,TRUE,FALSE,NULL); assert(ready);
        LmbChildHandle handles[]={{"LUMABRI_HOME_LISTEN_HANDLE",pair[1]},{"LUMABRI_READY_HANDLE",ready}};
        snprintf(text,sizeof text,"%d",port+(int)wrong);
        const char *args[]={"--worker",text};
        LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=2,.handles=handles,.nhandles=2,
            .error=GetStdHandle(STD_ERROR_HANDLE)};
        LmbOwnedProcess child={0}; assert(!lmb_owned_process_start(&child,&launch)); CloseHandle(pair[1]);
        assert(!lmb_home_export_listener(listener,GetProcessId(child.process),pair[0]));
        if (!wrong) {
            assert(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0);
            lmb_socket_close(listener); listener=LMB_INVALID_SOCKET;
            char target[64]; snprintf(target,sizeof target,"127.0.0.1:%d",port);
            LmbSocket client=lmb_connect(target); assert(client!=LMB_INVALID_SOCKET);
            assert(!lmb_write_full(client,"PING",4));
            char reply[2]; assert(!lmb_socket_read_full(client,reply,2) && !memcmp(reply,"OK",2));
            lmb_socket_close(client);
        }
        DWORD code; assert(!lmb_owned_process_wait(&child,5000,&code) && code==(wrong ? 64 : 0));
        assert(!lmb_owned_process_close(&child));
        if (listener!=LMB_INVALID_SOCKET) lmb_socket_close(listener);
        CloseHandle(pair[0]); CloseHandle(ready);
    }
    puts("NATIVE HOUSEHOLD NETWORK: PASS (interface inventory, explicit address, fixed port range, private cross-process listener handoff, wrong endpoint refusal)");
    return 0;
}
