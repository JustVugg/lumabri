/* The actual native lumabri.exe serving the web page: static assets, sign-in,
 * permissions, the private access store, history, a streamed chat turn
 * against an encrypted fixture host, the routes that are not available on
 * Windows yet, and `lumabri start` end to end with the real tracker.
 * No production household, browser or model is used by this test. */
#define NTDDI_VERSION 0x0A000006
#include "src/runtime/lumabri_owned_process_windows.h"
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_host_client.h"
#include "lumabri_home.h"
#include "src/runtime/lumabri_resident_plan_windows.h"
#include <assert.h>
static volatile sig_atomic_t g_stopping;
static double nowd(void) {return GetTickCount64()/1000.;}
#include "src/runtime/lumabri_host_protocol.h"

/* ---- an approved resident host that answers two turns ---- */
typedef struct {LmbSocket listener;char root[65];unsigned turns;} Host;
static void *host_serve(void *arg) {
    Host *s=arg;LmbHostEngine engine={.greedy_only=1,.numeric_abi=1};strcpy(engine.numeric_class,"fixture-f32");
    HostState h={.engine=&engine,.model_type="olmoe",.engine_kind="olmoe",.max_new=256,.max_frame=65536,.model_root=s->root,.slots=2};
    for(unsigned connection=0;connection<2;connection++) {
        assert(lmb_socket_wait(s->listener,0,30000)==1);
        LmbSocket fd=lmb_socket_accept(s->listener,NULL,NULL);assert(fd!=LMB_INVALID_SOCKET);
        assert(!lmb_socket_timeout(fd,5000) && !host_read_hello(fd,&h) && !host_greet(fd,&h,0));
        LmbBuf captured={0};HostInput input={.capture=&captured};
        while(!input.active) {
            LmbMsg m={0};assert(!lmb_recv_bounded(fd,&m,65536,10000));
            assert(m.op==LMB_HOST_STREAM && !m.body_len && m.pay_len && !host_input(&input,&engine,&h,m.pay,m.pay_len));
            lmb_msg_free(&m);
        }
        assert(!lmb_buf_bytes(&captured,"",1));char *payload=(char *)captured.p;
        if(!connection) assert(strstr(payload,"first caf\xc3\xa9") && !strstr(payload,"reply one"));
        else assert(strstr(payload,"first caf\xc3\xa9") && strstr(payload,"reply one") && strstr(payload,"second question"));
        const char *text=connection ? "reply two" : "reply one";char data[256];
        snprintf(data,sizeof data,"DATA %s %zu\n%s\nDONE %s STAT 8 1 PERF1 8 7 0.1 0.7 0.8\n",input.request_id,strlen(text),text,input.request_id);
        /* Exercise arbitrary encrypted DATA/header fragmentation. */
        for(size_t off=0;off<strlen(data);off+=3) {
            size_t n=strlen(data)-off;if(n>3) n=3;
            assert(!lmb_send(fd,LMB_HOST_STREAM,NULL,0,data+off,(uint32_t)n));
        }
        free(captured.p);s->turns++;
        /* The gateway closes its host connection when the turn is over. */
        LmbMsg m={0};assert(lmb_recv_bounded(fd,&m,4096,10000));lmb_msg_free(&m);lmb_close(fd);
    }
    return NULL;
}

/* ---- the binary under test ---- */
static wchar_t g_binary[4096];
static HANDLE spawn(const wchar_t *arguments,HANDLE output) {
    wchar_t line[8192];swprintf(line,8192,L"\"%ls\" %ls",g_binary,arguments);
    STARTUPINFOW start={.cb=sizeof start,.dwFlags=STARTF_USESTDHANDLES};PROCESS_INFORMATION pi;
    HANDLE null=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        &(SECURITY_ATTRIBUTES){sizeof(SECURITY_ATTRIBUTES),NULL,TRUE},OPEN_EXISTING,0,NULL);
    assert(null!=INVALID_HANDLE_VALUE);
    start.hStdInput=null;start.hStdOutput=output ? output : null;start.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    assert(CreateProcessW(g_binary,line,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&start,&pi));
    CloseHandle(null);CloseHandle(pi.hThread);return pi.hProcess;
}
/* Runs to completion; standard output (bounded) is returned to the caller. */
static int run(const wchar_t *arguments,char *out,size_t cap) {
    SECURITY_ATTRIBUTES inherit={sizeof inherit,NULL,TRUE};HANDLE reader,writer;
    assert(CreatePipe(&reader,&writer,&inherit,1u<<16) && SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0));
    HANDLE process=spawn(arguments,writer);CloseHandle(writer);
    size_t used=0;DWORD got;char bytes[4096];
    while(ReadFile(reader,bytes,sizeof bytes,&got,NULL) && got)
        if(out && used+got<cap) {memcpy(out+used,bytes,got);used+=got;}
    if(out) out[used]=0;
    CloseHandle(reader);assert(WaitForSingleObject(process,120000)==WAIT_OBJECT_0);
    DWORD code=1;assert(GetExitCodeProcess(process,&code));CloseHandle(process);return (int)code;
}
static unsigned free_port(void) {
    LmbSocket probe=lmb_listen(0);assert(probe!=LMB_INVALID_SOCKET);
    struct sockaddr_in address;LmbSocketLength size=sizeof address;
    assert(!getsockname(probe,(struct sockaddr *)&address,&size));lmb_close(probe);return ntohs(address.sin_port);
}

/* ---- one HTTP exchange, the way a browser sends it ---- */
static char g_reply[1u<<20];
static int http_try(unsigned port,const char *method,const char *path,const char *token,const char *host,const char *body) {
    LmbSocket fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(fd!=LMB_INVALID_SOCKET);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons((uint16_t)port);
    if(connect(fd,(struct sockaddr *)&address,sizeof address)) {closesocket(fd);return -1;}
    char request[8192],authorization[160]="",local[64];snprintf(local,sizeof local,"127.0.0.1:%u",port);
    if(token) snprintf(authorization,sizeof authorization,"Authorization: Bearer %s\r\n",token);
    int n=snprintf(request,sizeof request,"%s %s HTTP/1.1\r\nHost: %s\r\n%s",method,path,host ? host : local,authorization);
    if(body) n+=snprintf(request+n,sizeof request-(size_t)n,"Content-Type: application/json\r\nContent-Length: %zu\r\n",strlen(body));
    n+=snprintf(request+n,sizeof request-(size_t)n,"\r\n%s",body ? body : "");
    assert(n>0 && (size_t)n<sizeof request && send(fd,request,n,0)==n);
    size_t used=0;DWORD timeout=60000;setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof timeout);
    for(;;) {
        int got=recv(fd,g_reply+used,(int)(sizeof g_reply-1-used),0);
        if(got<=0) break;
        used+=(size_t)got;assert(used<sizeof g_reply-1);
    }
    g_reply[used]=0;closesocket(fd);
    return used>12 ? atoi(g_reply+9) : -1;
}
static int http(unsigned port,const char *method,const char *path,const char *token,const char *body) {
    return http_try(port,method,path,token,NULL,body);
}
static int has(const char *text) {return strstr(g_reply,text)!=NULL;}
/* Every delta of the last reply, decoded and joined as the page does. */
static void streamed(char *out,size_t cap) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t used=0;
    for(const char *at=g_reply;(at=strstr(at,"event: delta\ndata: {\"bytes\":\""));) {
        at+=29;uint32_t bits=0;unsigned held=0;
        for(;*at && *at!='"' && *at!='=';at++) {
            const char *digit=strchr(alphabet,*at);assert(digit);
            bits=(bits<<6)|(uint32_t)(digit-alphabet);held+=6;
            if(held>=8) {held-=8;assert(used+1<cap);out[used++]=(char)(bits>>held);}
        }
    }
    out[used]=0;
}
static void wait_listening(unsigned port) {
    for(unsigned i=0;i<200;i++) {if(http(port,"GET","/",NULL,NULL)==200) return;Sleep(50);}
    assert(!"the gateway did not start listening");
}
static void token_line(char *text) {
    char *end=text+strcspn(text,"\r\n");*end=0;
    assert(strlen(text)>66 && strchr(text,'.') && strlen(strchr(text,'.')+1)==64);
}
/* Owner and LocalSystem only, protected from inheritance, one hard link. */
static void assert_private(const char *path,int directory) {
    LmbPrivateSecurity security;assert(!lmb_private_security(&security));
    HANDLE h=directory ? INVALID_HANDLE_VALUE : lmb_private_open(path,0,0,NULL);
    if(directory) {
        wchar_t wide[1200];assert(MultiByteToWideChar(CP_UTF8,0,path,-1,wide,1200));
        h=CreateFileW(wide,READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    }
    assert(h!=INVALID_HANDLE_VALUE && !lmb_private_validate(h,security.user->User.Sid,directory));
    CloseHandle(h);lmb_private_security_free(&security);
}
/* What a careless copy or a hostile program would do: let Everyone read it. */
static void widen(const char *path) {
    wchar_t wide[1200],sddl[]=L"D:(A;;FA;;;WD)";PSECURITY_DESCRIPTOR descriptor=NULL;BOOL present,defaulted;PACL acl;
    assert(MultiByteToWideChar(CP_UTF8,0,path,-1,wide,1200));
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,NULL) &&
        GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted) && present);
    assert(!SetNamedSecurityInfoW(wide,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|UNPROTECTED_DACL_SECURITY_INFORMATION,NULL,NULL,acl,NULL));
    LocalFree(descriptor);
}

int wmain(int argc,wchar_t **argv) {
    assert(argc==2 && wcslen(argv[1])<4000);wcscpy(g_binary,argv[1]);
    wchar_t temp[1024],profile[1200];assert(GetTempPathW(1024,temp));
    swprintf(profile,1200,L"%lslumabri-web-%lu-%llu",temp,GetCurrentProcessId(),GetTickCount64());
    assert(CreateDirectoryW(profile,NULL) && SetEnvironmentVariableW(L"LOCALAPPDATA",profile));
    assert(!_putenv_s("LUMABRI_PEER_KEY","") && !_putenv_s("LUMABRI_KNOWN_HOSTS","") && !_putenv_s("LUMABRI_HOME_FOREGROUND","") &&
        !_putenv_s("LUMABRI_TOKEN","native-web-gateway-gate") && !_putenv_s("LUMABRI_ENCRYPT","1") && !lmb_secure_init());
    char access[1200],path[1400],out[4096],token[128],other[128];
    assert(!lmb_private_default_path(access,sizeof access,"api-access"));

    /* Credentials: shown once, stored as a digest in an owner-only record. */
    assert(!run(L"api user-add web",token,sizeof token));token_line(token);
    assert(!run(L"api user-add other",other,sizeof other));token_line(other);
    assert(run(L"api user-add web",NULL,0)==1 && run(L"api user-add Not_Valid",NULL,0)==1);
    assert_private(access,1);snprintf(path,sizeof path,"%s\\web.user",access);assert_private(path,0);
    uint8_t record[256];size_t length;assert(!lmb_private_blob_read(path,record,sizeof record,&length) && length==76);
    for(size_t i=0;i+64<=length;i++) assert(memcmp(record+i,strchr(token,'.')+1,64));

    /* An approved resident plan whose host is this test. */
    Host host={0};memset(host.root,'a',64);host.listener=lmb_listen(0);assert(host.listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in address;LmbSocketLength size=sizeof address;
    assert(!getsockname(host.listener,(struct sockaddr *)&address,&size));
    LmbResidentPlan *plan=calloc(1,sizeof *plan);assert(plan);
    plan->context=2048;plan->sessions=1;plan->max_new=64;plan->preparation_seconds=1;plan->prepared_at=1;
    plan->execution.count=1;plan->execution.layers=2;
    strcpy(plan->execution.nodes[0].name,"fixture");strcpy(plan->execution.nodes[0].address,"127.0.0.1:1");
    plan->execution.nodes[0].end=2;plan->execution.nodes[0].edge=1;plan->execution.nodes[0].reserved_bytes=1u<<20;
    snprintf(plan->host,sizeof plan->host,"127.0.0.1:%u",ntohs(address.sin_port));
    strcpy(plan->tracker,"127.0.0.1:1");strcpy(plan->model,"home-fixture-model");
    lmb_hex(plan->host_key,g_sec_pk,32);memcpy(plan->root,host.root,65);
    lmb_random(plan->allocation,32);memcpy(plan->peer_keys[0],g_sec_pk,32);
    assert(!home_resident_library_save(plan));
    char allocation[65],command[512];wchar_t arguments[1024];lmb_hex(allocation,plan->allocation,32);
    swprintf(arguments,1024,L"api grant web %hs --tracker 127.0.0.1:1",allocation);assert(!run(arguments,NULL,0));
    swprintf(arguments,1024,L"api grant web %hs --tracker 127.0.0.1:2",allocation);assert(run(arguments,NULL,0)==1);
    assert(!run(L"api list --tracker 127.0.0.1:1",out,sizeof out) && strstr(out,allocation));
    pthread_t thread;assert(!pthread_create(&thread,NULL,host_serve,&host));

    unsigned port=free_port();
    swprintf(arguments,1024,L"api serve --port %u --tracker 127.0.0.1:1",port);
    HANDLE job=CreateJobObjectW(NULL,NULL);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    assert(job && SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits));
    HANDLE server=spawn(arguments,NULL);assert(AssignProcessToJobObject(job,server));
    wait_listening(port);

    /* The page: compiled assets only, with the same headers as on POSIX. */
    assert(http(port,"GET","/",NULL,NULL)==200 && has("Content-Type: text/html") && has("<html") &&
        has("Content-Security-Policy: default-src 'none'; script-src 'self'") && has("X-Frame-Options: DENY") &&
        has("Cache-Control: no-store"));
    const char *assets[]={"/app.js","/app.css","/ui.js","/capacity.js","/logo.svg","/download.js","/download.css"};
    for(unsigned i=0;i<7;i++) assert(http(port,"GET",assets[i],NULL,NULL)==200);
    assert(http(port,"GET","/app.js?x=1",NULL,NULL)!=200 && http(port,"GET","/..\\home.conf",NULL,NULL)!=200);

    /* Loopback authority: Host and Origin are checked before anything else. */
    assert(http_try(port,"GET","/",NULL,"evil.example",NULL)==403 && has("origin_or_host_rejected"));
    assert(http(port,"GET","/api/v1/session",NULL,NULL)==401 && has("authentication_required"));
    snprintf(command,sizeof command,"web.%064d",0);
    assert(http(port,"GET","/api/v1/session",command,NULL)==401);

    /* A plain user: chat and history only. */
    assert(http(port,"GET","/api/v1/session",token,NULL)==200 && has("\"operator\":false,\"management\":false") &&
        has("\"platform\":\"windows\"") && has("\"model_download\""));
    assert(http(port,"GET","/api/v1/catalogue",token,NULL)==403 && has("operator_permission_required"));
    assert(http(port,"GET","/api/v1/workspace",token,NULL)==403);
    assert(http(port,"GET","/api/v1/preparation",token,NULL)==403 && has("management_permission_required"));
    assert(http(port,"GET","/api/v1/donor/approval",token,NULL)==403);
    assert(http(port,"POST","/api/v1/preparation",token,"{\"action\":\"status\",\"operation\":\"11\"}")==403);
    assert(http(port,"GET","/api/v1/models",token,NULL)==200 && has(allocation) && has("home-fixture-model"));
    assert(http(port,"GET","/api/v1/models",other,NULL)==200 && !has(allocation));

    /* What Windows does not serve yet says so, for a signed-in user only. */
    const char *missing[]={"/api/v1/downloads","/api/v1/capacity","/api/v1/portfolio-policy","/api/v1/elastic-policy",
        "/api/v1/demand","/api/v1/client-config","/v1/models","/api/v1/routing/x","/api/v1/allocations/x/control"};
    for(unsigned i=0;i<9;i++) {
        assert(http(port,"GET",missing[i],NULL,NULL)==401);
        assert(http(port,"GET",missing[i],token,NULL)==501 && has("not_available_on_windows_yet"));
    }
    assert(http(port,"POST","/v1/chat/completions",token,"{}")==501 && has("not_available_on_windows_yet"));
    assert(http(port,"GET","/api/v1/unknown",token,NULL)==400);

    /* Capabilities are separate records bound to the credential. */
    assert(!run(L"api operator-grant web",NULL,0));
    assert(http(port,"GET","/api/v1/session",token,NULL)==200 && has("\"operator\":true,\"management\":false"));
    assert(http(port,"GET","/api/v1/workspace",token,NULL)==200 && has("\"inventory_ok\":false") && has(allocation));
    assert(http(port,"GET","/api/v1/preparation",token,NULL)==403);
    assert(!run(L"api manage-grant web",NULL,0));
    snprintf(path,sizeof path,"%s\\web.manager",access);assert_private(path,0);
    assert(http(port,"GET","/api/v1/session",token,NULL)==200 && has("\"management\":true"));
    assert(http(port,"GET","/api/v1/preparation",token,NULL)==200 && has("\"operations\":[]"));
    assert(http(port,"GET","/api/v1/donor/approval",token,NULL)==200 && has("\"sharing\":false"));
    assert(http(port,"POST","/api/v1/preparation",token,
        "{\"action\":\"status\",\"operation\":\"1111111111111111111111111111111111111111111111111111111111111111\"}")==404);
    assert(http(port,"POST","/api/v1/donor/approval",token,"{\"action\":\"allow_repeat\"}")==501);
    assert(http(port,"GET","/api/v1/session",other,NULL)==200 && has("\"operator\":false,\"management\":false"));

    /* History is saved before any text reaches a model, per credential. */
    char conversation[2048],id[33];
    snprintf(conversation,sizeof conversation,"{\"revision\":0,\"conversation\":{\"model\":\"%s\",\"title\":\"gate\","
        "\"state\":\"pending\",\"messages\":[{\"role\":\"user\",\"content\":\"first caf\xc3\xa9\"}]}}",allocation);
    assert(http(port,"POST","/api/v1/conversations",other,conversation)==403 && has("model_not_authorized"));
    assert(http(port,"POST","/api/v1/conversations",token,conversation)==200 && has("\"revision\":1"));
    const char *created=strstr(g_reply,"{\"id\":\"");assert(created);memcpy(id,created+7,32);id[32]=0;
    assert(http(port,"GET","/api/v1/conversations",token,NULL)==200 && has(id) && has("\"title\":\"gate\""));
    assert(http(port,"GET","/api/v1/conversations",other,NULL)==200 && !has(id));
    snprintf(path,sizeof path,"/api/v1/conversations/%s",id);
    assert(http(port,"GET",path,token,NULL)==200 && has("first caf\xc3\xa9"));
    assert(http(port,"GET",path,other,NULL)==404);

    /* A streamed turn: identity and root are pinned by the saved plan. */
    char chat[2048];
    snprintf(chat,sizeof chat,"{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"first caf\xc3\xa9\"}],"
        "\"max_tokens\":8,\"stream_metrics\":true}",allocation);
    assert(http(port,"POST","/api/v1/chat",other,chat)==403 && has("model_not_authorized"));
    assert(http(port,"POST","/api/v1/chat",token,chat)==200 && has("Content-Type: text/event-stream") &&
        has("event: metrics") && has("event: delta\ndata: {\"bytes\":\"") && has("event: done") &&
        has("\"stats\":\"STAT 8 1 PERF1 8 7 0.1 0.7 0.8\"") && has("\"generated_tokens\":8") && !has("event: error"));
    /* Sent by the host as three-byte encrypted frames. */
    streamed(out,sizeof out);assert(!strcmp(out,"reply one"));
    snprintf(chat,sizeof chat,"{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"first caf\xc3\xa9\"},"
        "{\"role\":\"assistant\",\"content\":\"reply one\"},{\"role\":\"user\",\"content\":\"second question\"}],\"max_tokens\":8}",allocation);
    assert(http(port,"POST","/api/v1/chat",token,chat)==200 && has("event: done") && !has("event: metrics"));
    streamed(out,sizeof out);assert(!strcmp(out,"reply two"));
    pthread_join(thread,NULL);assert(host.turns==2);
    /* The host is gone: an honest failure, not a hang or a made-up answer. */
    lmb_close(host.listener);
    assert(http(port,"POST","/api/v1/chat",token,chat)==503 && has("host_unavailable_or_busy"));
    snprintf(chat,sizeof chat,"{\"model\":\"%064d\",\"messages\":[{\"role\":\"user\",\"content\":\"x\"}],\"max_tokens\":8}",1);
    assert(http(port,"POST","/api/v1/chat",token,chat)==403);

    /* A record that anyone else could read is not a credential any more. */
    snprintf(path,sizeof path,"%s\\web.user",access);widen(path);
    assert(http(port,"GET","/api/v1/session",token,NULL)==401);
    assert(!run(L"api revoke other",NULL,0) && http(port,"GET","/api/v1/session",other,NULL)==401);
    assert(TerminateProcess(server,0) && WaitForSingleObject(server,10000)==WAIT_OBJECT_0);
    CloseHandle(server);CloseHandle(job);free(plan);

    /* `lumabri start`: a household, the page and a private sign-in link. */
    swprintf(profile,1200,L"%lslumabri-start-%lu-%llu",temp,GetCurrentProcessId(),GetTickCount64());
    assert(CreateDirectoryW(profile,NULL) && SetEnvironmentVariableW(L"LOCALAPPDATA",profile));
    char household[16];snprintf(household,sizeof household,"%u",free_port());port=free_port();
    assert(!_putenv_s("LUMABRI_TOKEN","") && !_putenv_s("LUMABRI_ADVERTISE","127.0.0.1") && !_putenv_s("LUMABRI_HOME_PORT_BASE",household));
    assert(run(L"start --bogus",NULL,0)==2);
    swprintf(arguments,1024,L"start --no-browser --port %u --ram-gb 5",port);
    /* Sharing needs the Segment worker, which the contract build does not
     * install: that degrades the result to 1 and must not stop the page. */
    int started=run(arguments,out,sizeof out);
    if(started!=0 && started!=1) fprintf(stderr,"start exited %d: %s\n",started,out);
    assert((started==0 || started==1) && strstr(out,"created now") && strstr(out,"#token=local."));
    char link[160];snprintf(link,sizeof link,"http://127.0.0.1:%u/#token=local.",port);
    char *sign_in=strstr(out,link);assert(sign_in);
    snprintf(token,sizeof token,"%.70s",sign_in+strlen(link)-6);token_line(token);
    assert(http(port,"GET","/api/v1/session",token,NULL)==200 && has("\"operator\":true,\"management\":true"));
    assert(http(port,"GET","/api/v1/workspace",token,NULL)==200 && has("\"inventory_ok\":true"));
    assert(http(port,"GET","/api/v1/catalogue",token,NULL)==200 && has("\"models\":[]"));
    assert(!lmb_private_default_path(access,sizeof access,"api-access"));
    snprintf(path,sizeof path,"%s\\local.token",access);assert_private(path,0);
    snprintf(path,sizeof path,"%s\\local.user",access);assert_private(path,0);
    /* Starting again adopts the running page and hands out the same link. */
    char again[4096];int repeated=run(L"start --no-browser",again,sizeof again);
    assert((repeated==0 || repeated==1) && strstr(again,"(yours)") && strstr(again,link) && strstr(again,token));
    assert(!run(L"api stop",NULL,0) && http(port,"GET","/",NULL,NULL)<0);
    assert(!run(L"service stop",NULL,0));
    puts("NATIVE WEB GATEWAY: PASS (compiled page and headers, loopback Host check, owner-only credential records, "
         "digest-only storage, operator and management capabilities, per-credential history, streamed encrypted chat "
         "with pinned host, explicit not-available routes, widened record refused, lumabri start with private link, adoption, stop)");
    return 0;
}
