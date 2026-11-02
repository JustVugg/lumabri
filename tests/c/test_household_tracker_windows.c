/* Real Windows workspace -> keeper -> common tracker. Only disposable
 * identities, directories and loopback sockets are used. */
#define NTDDI_VERSION 0x0A000006
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_secure.h"
#include "lumabri_home_discovery.h"
#include "lumabri_segment_discovery.h"
#include "src/runtime/lumabri_home_settings_windows.h"
#include <assert.h>
#include "console_fixture_windows.h"

static char binary[6144];
static LmbOwnedProcess commands[12];
static unsigned command_count;
static void command(const char *action) {
    assert(command_count<12);
    const char *args[]={"service",action};LmbOwnedProcess *child=&commands[command_count++];
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=2,.error=GetStdHandle(STD_ERROR_HANDLE)};
    assert(!lmb_owned_process_start(child,&launch));DWORD code;
    /* Restart intentionally leaves a manager descendant alive. Observe the
     * CLI result here; retain its owned job until service stop below. */
    assert(WaitForSingleObject(child->process,30000)==WAIT_OBJECT_0 && GetExitCodeProcess(child->process,&code));
    if (code) fprintf(stderr,"Native service %s failed: %lu\n",action,code);
    assert(code==0);
}
static HomeServiceSnapshot status(const char *role) {
    HomeServiceSnapshot s;uint64_t until=GetTickCount64()+5000;
    while (home_service_query(role,HOME_SVC_STATUS,NULL,&s)) {assert(GetTickCount64()<until);Sleep(20);}
    return s;
}
static void remove_fixture(const wchar_t *directory) {
    wchar_t pattern[1200];assert(swprintf(pattern,1200,L"%ls\\*",directory)>0);
    WIN32_FIND_DATAW entry;HANDLE scan=FindFirstFileW(pattern,&entry);assert(scan!=INVALID_HANDLE_VALUE);
    do {
        if (!wcscmp(entry.cFileName,L".") || !wcscmp(entry.cFileName,L"..")) continue;
        assert(!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));
        wchar_t child[1400];assert(swprintf(child,1400,L"%ls\\%ls",directory,entry.cFileName)>0);
        if (entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) remove_fixture(child);
        else assert(DeleteFileW(child));
    } while (FindNextFileW(scan,&entry));
    FindClose(scan);assert(RemoveDirectoryW(directory));
}
static LmbSegAdvert advert(void) {
    LmbSegAdvert a={0};strcpy(a.peer_name,"native-range");strcpy(a.addr,"127.0.0.1:7800");
    strcpy(a.model,"native-tracker-fixture");memset(a.model_root,1,32);memset(a.tokenizer_root,2,32);
    a.layer_end=2;a.max_context=128;a.max_rows=8;a.state_dtype=LMB_SEG_DTYPE_F32;a.state_width=8;
    a.max_sessions=2;a.resident_ram_bytes=64u<<20;
    a.capabilities=LMB_SEG_CAP_RANGE_NATIVE|LMB_SEG_CAP_MULTI_SESSION|LMB_SEG_CAP_CPU;
    strcpy(a.engine_id,"olmoe");strcpy(a.state_schema,"kv-standard-v1");strcpy(a.numeric_class,"olmoe-f32-strict-tiny");
    assert(lmb_seg_advert_valid(&a));return a;
}
static LmbSocket publish(const char *address,const LmbSegAdvert *a,LmbSegOwner *owner) {
    LmbSocket fd=lmb_connect_ms_io(address,1500,1500);assert(fd!=LMB_INVALID_SOCKET && !lmb_auth(fd));
    uint8_t nonce[32],message[512],sig[64],*wire=NULL;uint32_t size;
    assert(!lmb_request_challenge(fd,nonce) && !lmb_seg_advert_encode(a,&wire,&size));
    size_t n=lmb_peer_auth_msg(nonce,a->peer_name,a->model,a->addr,message,sizeof message);assert(n);
    lmb_sign(sig,message,n,g_sec_sk);LmbBuf body={wire,size,size};
    assert(!lmb_buf_peer_auth(&body,g_sec_pk,sig) && !lmb_send(fd,LMB_SEG_REGISTER,body.p,(uint32_t)body.len,NULL,0));free(body.p);
    LmbMsg reply={0};assert(!lmb_recv(fd,&reply) && reply.op==LMB_SEG_REGISTER_R &&
        !lmb_seg_registration_reply_decode(reply.body,reply.body_len,owner));lmb_msg_free(&reply);return fd;
}
static uint64_t routes(const char *address,const LmbSegAdvert *a,unsigned count) {
    LmbSegQuery q={0};strcpy(q.model,a->model);memcpy(q.model_root,a->model_root,32);
    memcpy(q.tokenizer_root,a->tokenizer_root,32);q.layer_end=2;q.context_tokens=128;q.rows=1;
    q.state_dtype=a->state_dtype;q.state_width=a->state_width;q.required_capabilities=LMB_SEG_CAP_RANGE_NATIVE;
    strcpy(q.engine_id,a->engine_id);strcpy(q.state_schema,a->state_schema);strcpy(q.numeric_class,a->numeric_class);
    uint64_t until=GetTickCount64()+5000;LmbSegRouteSnapshot r={0};
    for (;;) {
        assert(!lmb_seg_routes_fetch(address,&q,&r));
        if (r.count==count) break;
        assert(GetTickCount64()<until);Sleep(20);
    }
    return r.route_generation;
}
int wmain(int argc,wchar_t **argv) {
    assert(argc==2);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    wchar_t temp[512],directory[700];assert(GetTempPathW(512,temp));
    assert(swprintf(directory,700,L"%lslumabri-tracker-ui-%lu-%llu-\x00e8",temp,GetCurrentProcessId(),GetTickCount64())>0);
    assert(CreateDirectoryW(directory,NULL) && SetEnvironmentVariableW(L"LOCALAPPDATA",directory));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[1],-1,binary,sizeof binary,NULL,NULL));
    assert(!_putenv_s("LUMABRI_ADVERTISE","127.0.0.1") && !_putenv_s("LUMABRI_ENCRYPT","1") &&
        !_putenv_s("LUMABRI_PEER_KEY","") && !_putenv_s("LUMABRI_KNOWN_HOSTS","") &&
        !_putenv_s("LUMABRI_HOME_FOREGROUND","") && !_putenv_s("LUMABRI_TOKEN","") && !lmb_secure_init());
    LmbSocket occupied=LMB_INVALID_SOCKET;struct sockaddr_in addr;
    for (unsigned attempt=0;attempt<100;attempt++) {
        occupied=lmb_listen(0);assert(occupied!=LMB_INVALID_SOCKET);
        int size=sizeof addr;assert(!getsockname(occupied,(struct sockaddr *)&addr,&size));
        if (ntohs(addr.sin_port)<=65536-LMB_HOME_PORT_COUNT) break;
        lmb_close(occupied);occupied=LMB_INVALID_SOCKET;
    }
    assert(occupied!=LMB_INVALID_SOCKET);
    char port[16];snprintf(port,sizeof port,"%u",ntohs(addr.sin_port));assert(!_putenv_s("LUMABRI_HOME_PORT_BASE",port));
    WinUi ui;winui_open_args(&ui,binary,L"","Create a household");
    winui_send_keys(ui.input,"jjjj\r");winui_expect(ui.capture,"Could not create the household");
    HomeSettings settings;assert(!home_settings_load_windows(&settings) && !settings.owner && !settings.tracker[0]);
    winui_send_keys(ui.input,"\r");Sleep(100);winui_close(&ui);command("stop");lmb_close(occupied);

    winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjj\r");
    winui_expect(ui.capture,"Connect your other computers");
    assert(!home_settings_load_windows(&settings) && settings.owner && strlen(settings.token)==64);
    assert(!winui_contains(ui.capture,settings.token));winui_send_keys(ui.input,"r");winui_expect(ui.capture,settings.token);
    winui_send_keys(ui.input,"\r");Sleep(100);winui_close(&ui);
    HomeServiceSnapshot first=status("tracker"),manager=status("manager");
    assert(first.state==HOME_SVC_RUNNING && !strcmp(first.tracker,settings.tracker));
    assert(!home_settings_activate_windows(&settings) && !home_connection_check_identity_windows(settings.tracker,g_sec_pk));
    assert(!_putenv_s("LUMABRI_TOKEN","wrong-household-key") && home_connection_check_windows(settings.tracker)==-2);
    assert(!home_settings_activate_windows(&settings));
    LmbHomeFound found[LMB_HOME_DISC_MAX];uint8_t nonce[16];lmb_random(nonce,sizeof nonce);
    int n=lmb_home_find(found,nonce),ours=0;
    /* Broadcast can arrive before the loopback query. The reply advertises
     * its actual source interface; identity, port and handshake must agree. */
    for (int i=0;i<n;i++) if (!memcmp(found[i].identity,g_sec_pk,32)) {
        const char *colon=strrchr(found[i].address,':');
        assert(colon && !strcmp(colon+1,port) && !home_connection_check_identity_windows(found[i].address,g_sec_pk));ours=1;
    }
    if (!ours) fprintf(stderr,"No matching native beacon among %d replies\n",n);
    assert(ours);
    LmbSegAdvert a=advert();LmbSegOwner owner;LmbSocket registration=publish(settings.tracker,&a,&owner);
    uint64_t generation=routes(settings.tracker,&a,1);assert(generation);
    command("restart");HomeServiceSnapshot same=status("tracker"),next=status("manager");
    assert(!memcmp(first.instance,same.instance,32) && memcmp(manager.instance,next.instance,32));
    assert(routes(settings.tracker,&a,1)==generation);
    winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjj\r");
    winui_expect(ui.capture,"Connect your other computers");winui_send_keys(ui.input,"\r");Sleep(100);winui_close(&ui);
    same=status("tracker");assert(!memcmp(first.instance,same.instance,32));
    /* Direct routes retain their bounded heartbeat lease after a control
     * disconnect; shutting down this owner must still close the listener. */
    lmb_close(registration);command("stop");
    winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjj\r");
    winui_expect(ui.capture,"Connect your other computers");winui_send_keys(ui.input,"\r");Sleep(100);winui_close(&ui);
    HomeSettings restored;assert(!home_settings_load_windows(&restored) && !strcmp(settings.token,restored.token));
    next=status("tracker");assert(memcmp(first.instance,next.instance,32));
    assert(home_service_query("tracker",HOME_SVC_STOP,&first,&same));
    registration=publish(settings.tracker,&a,&owner);assert(routes(settings.tracker,&a,1)>generation);
    lmb_close(registration);command("stop");
    for (unsigned i=0;i<command_count;i++) {
        assert(!lmb_owned_process_stop(&commands[i],0,5000) && !lmb_owned_process_close(&commands[i]));
    }
    remove_fixture(directory);
    puts("WINDOWS HOUSEHOLD TRACKER: PASS (real TUI create, port conflict rollback, hidden key, encrypted auth, LAN beacon, signed Segment discovery, TUI detach, manager restart, durable route generation, stale stop refusal)");
    return 0;
}
