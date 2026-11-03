/* Actual console and encrypted client under deterministic network faults.
 * Real OLMoE inference is exercised separately by test_donor_windows. */
#define NTDDI_VERSION 0x0A000006
#include "src/runtime/lumabri_owned_process_windows.h"
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_host_client.h"
#include <assert.h>
#include "console_fixture_windows.h"
static volatile sig_atomic_t g_stopping;
static double nowd(void) {return GetTickCount64()/1000.;}
#include "src/runtime/lumabri_host_protocol.h"

typedef struct {LmbSocket listener;HANDLE partial,closed;char root[65];unsigned step;} Server;
static void send_text(LmbSocket fd,const char *text) {
    assert(!lmb_send(fd,LMB_HOST_STREAM,NULL,0,text,(uint32_t)strlen(text)));
}
static void *serve(void *arg) {
    Server *s=arg;LmbHostEngine engine={.greedy_only=1,.numeric_abi=1};strcpy(engine.numeric_class,"fixture-f32");
    HostState h={.engine=&engine,.model_type="olmoe",.engine_kind="olmoe",.max_new=256,.max_frame=65536,.model_root=s->root,.slots=2};
    for(unsigned connection=0;connection<5;connection++) {
        assert(lmb_socket_wait(s->listener,0,20000)==1);
        LmbSocket fd=lmb_socket_accept(s->listener,NULL,NULL);assert(fd!=LMB_INVALID_SOCKET);
        assert(!lmb_socket_timeout(fd,5000) && !host_read_hello(fd,&h));
        assert(!host_greet(fd,&h,connection==1));
        if(connection<2) {LmbMsg m={0};assert(lmb_recv_bounded(fd,&m,4096,5000));lmb_msg_free(&m);lmb_close(fd);continue;}
        for(unsigned turn=0;turn<(connection==3 ? 2u : 1u);turn++) {
            LmbBuf captured={0};HostInput input={.capture=&captured};
            while(!input.active) {
                LmbMsg m={0};assert(!lmb_recv_bounded(fd,&m,65536,10000));
                assert(m.op==LMB_HOST_STREAM && !m.body_len && m.pay_len && !host_input(&input,&engine,&h,m.pay,m.pay_len));
                lmb_msg_free(&m);
            }
            assert(!lmb_buf_bytes(&captured,"",1));char *payload=(char *)captured.p;
            if(connection==2) {
                assert(strstr(payload,"first café"));
                char data[128];snprintf(data,sizeof data,"DATA %s 13\npartial reply\n",input.request_id);send_text(fd,data);
                SetEvent(s->partial);LmbMsg m={0};assert(lmb_recv_bounded(fd,&m,65536,10000));lmb_msg_free(&m);SetEvent(s->closed);
            } else {
                assert(!strstr(payload,"first café") && !strstr(payload,"partial reply"));
                if(connection==3 && !turn) assert(strstr(payload,"second") && !strstr(payload,"reply two"));
                if(connection==3 && turn) assert(strstr(payload,"second") && strstr(payload,"reply two") && strstr(payload,"third"));
                if(connection==4) assert(strstr(payload,"fresh") && !strstr(payload,"second") && !strstr(payload,"third"));
                const char *text=turn ? "text DONE 999\n" : "reply two";char data[256];
                snprintf(data,sizeof data,"DATA %s %zu\n%s\nDONE %s STAT 8 1 PERF1 8 7 0.1 0.7 0.8\n",input.request_id,strlen(text),text,input.request_id);
                /* Exercise arbitrary encrypted DATA/header fragmentation. */
                for(size_t off=0;off<strlen(data);off+=3) {
                    size_t n=strlen(data)-off;if(n>3) n=3;
                    assert(!lmb_send(fd,LMB_HOST_STREAM,NULL,0,data+off,(uint32_t)n));
                }
            }
            free(captured.p);s->step++;
        }
        if(connection!=2) {LmbMsg m={0};assert(lmb_recv_bounded(fd,&m,4096,10000));lmb_msg_free(&m);}
        lmb_close(fd);
    }
    return NULL;
}
static void reset_capture(WinUi *ui) {
    EnterCriticalSection(&ui->capture->mutex);ui->capture->used=0;ui->capture->text[0]=0;LeaveCriticalSection(&ui->capture->mutex);
}
int wmain(int argc,wchar_t **argv) {
    assert(argc==2);char binary[4096];
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[1],-1,binary,sizeof binary,NULL,NULL));
    wchar_t temp[1024],profile[1200];assert(GetTempPathW(1024,temp));
    swprintf(profile,1200,L"%lslumabri-chat-%lu-%llu",temp,GetCurrentProcessId(),GetTickCount64());
    assert(CreateDirectoryW(profile,NULL) && SetEnvironmentVariableW(L"LOCALAPPDATA",profile));
    assert(!_putenv_s("LUMABRI_PEER_KEY","") && !_putenv_s("LUMABRI_KNOWN_HOSTS","") &&
        !_putenv_s("LUMABRI_TOKEN","native-chat-fault-gate") && !_putenv_s("LUMABRI_ENCRYPT","1") && !lmb_secure_init());
    Server server={0};memset(server.root,'a',64);server.partial=CreateEventW(NULL,TRUE,FALSE,NULL);server.closed=CreateEventW(NULL,TRUE,FALSE,NULL);
    server.listener=lmb_listen(0);assert(server.listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in address;LmbSocketLength size=sizeof address;
    assert(!getsockname(server.listener,(struct sockaddr *)&address,&size));
    char key[65];lmb_hex(key,g_sec_pk,32);pthread_t thread;assert(!pthread_create(&thread,NULL,serve,&server));
    wchar_t args[1024];char wrong[65];memset(wrong,'b',64);wrong[64]=0;
    swprintf(args,1024,L"chat --host 127.0.0.1:%u --host-key %hs --host-root %hs --max-new 8",ntohs(address.sin_port),key,wrong);
    WinUi ui;winui_open_args(&ui,binary,args,"conversation");winui_send_keys(ui.input,"never transmit\r");
    winui_expect(ui.capture,"No message sent.");winui_close(&ui);
    swprintf(args,1024,L"chat --host 127.0.0.1:%u --host-key %hs --host-root %hs --max-new 8",ntohs(address.sin_port),key,server.root);
    winui_open_args(&ui,binary,args,"conversation");winui_send_keys(ui.input,"first café\r");
    winui_expect(ui.capture,"Host busy.");winui_send_keys(ui.input,"\r");
    assert(WaitForSingleObject(server.partial,10000)==WAIT_OBJECT_0);
    winui_expect(ui.capture,"partial reply");winui_send_keys(ui.input,"\x1b");winui_expect(ui.capture,"Cancelled.");
    assert(WaitForSingleObject(server.closed,3000)==WAIT_OBJECT_0);
    winui_send_keys(ui.input,"second\r");winui_expect(ui.capture,"Reply 2 ·");
    assert(winui_contains(ui.capture,"first café") && winui_contains(ui.capture,"Reply interrupted"));
    reset_capture(&ui);winui_send_keys(ui.input,"third\r");winui_expect(ui.capture,"text DONE 999");winui_expect(ui.capture,"Reply 3 ·");
    assert(winui_contains(ui.capture,"text DONE 999"));
    winui_send_keys(ui.input,"/ne\t\r");winui_expect(ui.capture,"New private conversation.");
    reset_capture(&ui);winui_send_keys(ui.input,"fresh\r");winui_expect(ui.capture,"Reply 4 ·");winui_close(&ui);
    pthread_join(thread,NULL);assert(server.step==4);CloseHandle(server.partial);CloseHandle(server.closed);lmb_close(server.listener);
    puts("NATIVE CHAT: PASS (pinned root refusal before text, BUSY, Unicode, streamed fragments, cancellation closes socket, partial exclusion, private multi-turn replay, /new, no checkpoint startup)");return 0;
}
