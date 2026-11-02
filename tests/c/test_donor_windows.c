/* Exercise actual native lumabri.exe, same-user service IPC and resident
 * Segment child. The checkpoint source/oracle is deliberately reduced. */
#define NTDDI_VERSION 0x0A000006
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_home_net.h"
#include "src/runtime/lumabri_allocation_windows.h"
#include <assert.h>
#include "segment_worker_fixture_windows.h"
#include "console_fixture_windows.h"
#include <signal.h>
static volatile sig_atomic_t g_stopping;
static double nowd(void) {return GetTickCount64()/1000.;}
#include "src/runtime/lumabri_host_protocol.h"
#include "host_client_fixture_windows.h"
static uint32_t host_port;

static LmbOwnedProcess owners[16];static unsigned nowners;
static char binary[4096],profile[4096],endpoint[100],keyenv[4200],knownenv[4200];
static DWORD command(const char *const *args,size_t count) {
    const char *environment[]={keyenv,knownenv};
    assert(nowners<16);LmbOwnedProcess *child=&owners[nowners++];
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=count,.environment=environment,.nenvironment=2,
        .output=GetStdHandle(STD_ERROR_HANDLE),.error=GetStdHandle(STD_ERROR_HANDLE)};
    assert(!lmb_owned_process_start(child,&launch));
    assert(WaitForSingleObject(child->process,15000)==WAIT_OBJECT_0);
    DWORD code;assert(GetExitCodeProcess(child->process,&code));return code;
}
static HomeServiceSnapshot status(void) {
    HomeServiceSnapshot s;uint64_t until=GetTickCount64()+5000;
    while (home_service_query("donor",HOME_SVC_STATUS,NULL,&s)) {assert(GetTickCount64()<until);Sleep(10);}
    return s;
}
static unsigned receive(LmbSocket fd,const LmbHomeOffer *o) {
    LmbMsg m={0};assert(!lmb_recv_bounded(fd,&m,4096,3000));
    LmbCur c={m.body,m.body_len,0};uint32_t version,phase,segment,host;uint8_t id[32];char reason[256];
    assert(m.op==LMB_HOME_STATUS && !m.pay_len && !lmb_cur_u32(&c,&version) && version==LMB_HOME_VERSION &&
        !lmb_cur_bytes(&c,id,32) && !memcmp(id,o->id,32) && !lmb_cur_u32(&c,&phase) &&
        !lmb_cur_u32(&c,&segment) && !lmb_cur_u32(&c,&host) && !lmb_cur_str(&c,reason,sizeof reason) && c.off==c.len);
    if (phase==LMB_HOME_FAILED) fprintf(stderr,"donor failed: %s\n",reason);
    host_port=host;lmb_msg_free(&m);return phase;
}
static LmbSocket offer(const LmbHomeOffer *o,unsigned phase) {
    LmbSocket fd=lmb_connect_ms_io(endpoint,2000,2000);assert(fd!=LMB_INVALID_SOCKET && !lmb_auth(fd));
    LmbBuf b={0};assert(!lmb_home_offer_pack(&b,o) && !lmb_send(fd,LMB_HOME_OFFER,b.p,(uint32_t)b.len,NULL,0));free(b.p);
    assert(receive(fd,o)==phase);return fd;
}
static void approved(LmbSocket fd,const LmbHomeOffer *o,int use_ui) {
    HomeServiceSnapshot before=status(),after;
    assert(before.phase==LMB_HOME_PENDING && !before.segment_pid);
    uint64_t previously_reserved=before.reserved_total;
    HomeServiceSnapshot stale=before;stale.revision++;
    assert(home_service_query("donor",HOME_SVC_ACCEPT,&stale,&after));
    if (use_ui) {
        WinUi ui;winui_open(&ui,binary);winui_expect(ui.capture,"Accept this request");
        assert(SUCCEEDED(ResizePseudoConsole(ui.console,(COORD){50,20})));
        winui_expect(ui.capture,"Resize to at least");winui_send_keys(ui.input,"\r");Sleep(150);
        assert(status().phase==LMB_HOME_PENDING);
        assert(SUCCEEDED(ResizePseudoConsole(ui.console,(COORD){104,38})));Sleep(200);
        winui_send_keys(ui.input,"\x1b[A\r");winui_expect(ui.capture,"Accepted; waiting for other computers");
        winui_close(&ui);after=status();
    } else assert(!home_service_query("donor",HOME_SVC_ACCEPT,&before,&after));
    assert(after.phase==LMB_HOME_ACCEPTED && after.reserved_total==previously_reserved+o->ram_bytes && !after.segment_pid);
    assert(receive(fd,o)==LMB_HOME_ACCEPTED);
    assert(home_service_query("donor",HOME_SVC_ACCEPT,&before,&after));
    assert(!lmb_send(fd,LMB_HOME_COMMIT,o->id,32,NULL,0));
    assert(receive(fd,o)==LMB_HOME_LOADING);
    uint64_t until=GetTickCount64()+30000,pulse=0;int ready=0;
    while (!ready) {
        assert(GetTickCount64()<until);
        if (GetTickCount64()>pulse) {assert(!lmb_send(fd,LMB_HOME_PULSE,o->id,32,NULL,0));pulse=GetTickCount64()+1000;}
        if (lmb_socket_wait(fd,0,50)==1) {
            unsigned phase=receive(fd,o);assert(phase==LMB_HOME_LOADING || phase==LMB_HOME_SEGMENT_READY);
            ready=phase==LMB_HOME_SEGMENT_READY;
        }
    }
    do {after=status();Sleep(10);} while (after.phase==LMB_HOME_LOADING);
    assert(after.phase==LMB_HOME_SEGMENT_READY && after.retained==!o->runs_edge && after.segment_pid && after.reserved_total==previously_reserved+o->ram_bytes);
}
static LmbSegAdvert advert_for(WorkerFixture *f,const LmbHomeOffer *o) {
    char id[65],name[64];lmb_hex(id,o->id,32);snprintf(name,sizeof name,"home-%.12s-%u",id,o->begin);
    uint64_t until=GetTickCount64()+5000;
    for (;;) {
        pthread_mutex_lock(&f->lock);
        for (unsigned i=0;i<f->nadverts;i++) if (!strcmp(name,f->adverts[i].peer_name)) {
            LmbSegAdvert result=f->adverts[i];pthread_mutex_unlock(&f->lock);return result;
        }
        pthread_mutex_unlock(&f->lock);assert(GetTickCount64()<until);Sleep(10);
    }
}
static LmbNodeControl control(const LmbHomeOffer *o,uint32_t action,const LmbNodeControl *expected) {
    uint8_t body[64+LMB_NODE_CONTROL_REQUEST_BYTES];memcpy(body,o->id,32);memcpy(body+32,o->model_root,32);
    lmb_node_control_request(body+64,action,expected);
    LmbSocket fd=lmb_connect_ms_io(endpoint,2000,2000);assert(fd!=LMB_INVALID_SOCKET && !lmb_auth(fd));
    assert(!lmb_send(fd,LMB_HOME_NODE_CONTROL,body,sizeof body,NULL,0));LmbMsg m={0};
    assert(!lmb_recv_bounded(fd,&m,4096,2000) && m.op==LMB_HOME_NODE_CONTROL_R && m.body_len==64+LMB_NODE_CONTROL_REPLY_BYTES && !m.pay_len);
    LmbNodeControl state;assert(!memcmp(body,m.body,64) && !lmb_node_control_decode(m.body+64,LMB_NODE_CONTROL_REPLY_BYTES,&state));
    lmb_msg_free(&m);lmb_close(fd);return state;
}
static void retire(const LmbHomeOffer *o,const LmbNodeControl *seal) {
    uint64_t until=GetTickCount64()+5000;
 retry:
    uint8_t body[64+LMB_NODE_CONTROL_REQUEST_BYTES];memcpy(body,o->id,32);memcpy(body+32,o->model_root,32);
    lmb_node_control_request(body+64,LMB_NODE_CONTROL_QUERY,seal);
    LmbSocket fd=lmb_connect_ms_io(endpoint,2000,2000);assert(fd!=LMB_INVALID_SOCKET && !lmb_auth(fd));
    assert(!lmb_send(fd,LMB_HOME_RETIRED_RELEASE,body,sizeof body,NULL,0));LmbMsg m={0};
    int rc=lmb_recv_bounded(fd,&m,4096,3000);
    if ((rc || m.op!=LMB_HOME_RETIRED_RELEASE_R) && GetTickCount64()<until) {
        lmb_msg_free(&m);lmb_close(fd);Sleep(100);goto retry;
    }
    if (rc || m.op!=LMB_HOME_RETIRED_RELEASE_R) {
        HomeServiceSnapshot now=status();
        fprintf(stderr,"retire rc=%d op=%u phase=%u reserved=%llu pid=%llu detail=%s\n",rc,m.op,now.phase,
            (unsigned long long)now.reserved_total,(unsigned long long)now.segment_pid,now.detail);
    }
    assert(!rc && m.op==LMB_HOME_RETIRED_RELEASE_R && m.body_len==64+LMB_NODE_CONTROL_REPLY_BYTES && !m.pay_len);
    lmb_msg_free(&m);lmb_close(fd);
}
int wmain(int argc,wchar_t **wide) {
    assert(argc==3);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    char model[4096];wchar_t temp[1024],dir[1200];
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[1],-1,binary,sizeof binary,NULL,NULL));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[2],-1,model,sizeof model,NULL,NULL));
    assert(GetTempPathW(1024,temp));swprintf(dir,1200,L"%lslumabri-donor-\x00e8-%lu-%llu",temp,GetCurrentProcessId(),GetTickCount64());
    assert(CreateDirectoryW(dir,NULL) && SetEnvironmentVariableW(L"LOCALAPPDATA",dir));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,dir,-1,profile,sizeof profile,NULL,NULL));
    char p[4200],port[16];snprintf(p,sizeof p,"%s/parent.key",profile);assert(!_putenv_s("LUMABRI_PEER_KEY",p));
    snprintf(p,sizeof p,"%s/parent.known",profile);assert(!_putenv_s("LUMABRI_KNOWN_HOSTS",p));
    snprintf(keyenv,sizeof keyenv,"LUMABRI_PEER_KEY=%s/worker.key",profile);
    snprintf(knownenv,sizeof knownenv,"LUMABRI_KNOWN_HOSTS=%s/worker.known",profile);
    snprintf(port,sizeof port,"%u",(unsigned)(50000+GetCurrentProcessId()%10000));
    assert(!_putenv_s("LUMABRI_HOME_PORT_BASE",port) && !_putenv_s("LUMABRI_ADVERTISE","127.0.0.1") &&
        !_putenv_s("LUMABRI_RAM_RESERVE_MB","256") && !_putenv_s("LUMABRI_TOKEN","native-worker-test") &&
        !_putenv_s("LUMABRI_ENCRYPT","1") && !_putenv_s("LUMABRI_HOME_FOREGROUND","") && !lmb_secure_init());
    snprintf(endpoint,sizeof endpoint,"127.0.0.1:%u",(unsigned)atoi(port)+1);
    char tracker[100];WorkerFixture fixture;fixture_start(&fixture,tracker,sizeof tracker,model);
    char path[1024];uint8_t sk[64],pk[32];assert(!lmb_peer_identity(lmb_peer_key_path(path,sizeof path),sk,pk));
    memcpy(fixture.checkpoint.key,pk,32);memcpy(fixture.checkpoint.secret,sk,64);
    assert(!coli_olmoe_edge_adapter_register() && !coli_olmoe_segment_adapter_register());
    /* Join and configure via the real launcher, with no donor command or
     * inherited household key. The background keeper must inherit the saved
     * credential activated by the UI, not this test requester's environment. */
    assert(!_putenv_s("LUMABRI_PEER_KEY",strchr(keyenv,'=')+1) &&
        !_putenv_s("LUMABRI_KNOWN_HOSTS",strchr(knownenv,'=')+1) && !_putenv_s("LUMABRI_TOKEN",""));
    WinUi setup;winui_open_args(&setup,binary,L"","Join a household");
    winui_send_keys(setup.input,"j\r");winui_expect(setup.capture,"Household address (host:port)");
    winui_send_keys(setup.input,tracker);winui_send_keys(setup.input,"\r");
    winui_expect(setup.capture,"Household key from its owner");winui_send_keys(setup.input,"native-worker-test\r");
    winui_expect(setup.capture,tracker);Sleep(100);
    winui_send_keys(setup.input,"j\r");winui_expect(setup.capture,"Folder containing your models");
    winui_send_keys(setup.input,"\r");winui_expect(setup.capture,"Maximum RAM to share, in GB");
    winui_send_keys(setup.input,"\x15" "0.402653184\r");Sleep(200);
    winui_send_keys(setup.input,"kk\r");winui_expect(setup.capture,"share resources");
    HomeServiceSnapshot initial=status();assert(!initial.has_offer && !initial.reserved_total);
    winui_send_keys(setup.input,"\x1b");Sleep(200);winui_close(&setup);
    snprintf(p,sizeof p,"%s/parent.key",profile);assert(!_putenv_s("LUMABRI_PEER_KEY",p));
    snprintf(p,sizeof p,"%s/parent.known",profile);assert(!_putenv_s("LUMABRI_KNOWN_HOSTS",p));
    assert(!_putenv_s("LUMABRI_TOKEN","native-worker-test"));
    pthread_mutex_lock(&fixture.lock);LmbMachineReport reported=fixture.machine_report;
    assert(fixture.machine_reports>0);pthread_mutex_unlock(&fixture.lock);
    assert(reported.roles.known && !strcmp(reported.roles.segment,"olmoe") && !strcmp(reported.roles.edge,"olmoe") && !reported.roles.hybrid[0]);
    assert(reported.runtime_threads==1 && reported.ram_budget_bytes==initial.ram && !strcmp(reported.control_addr,endpoint));
    LmbHomeOffer o={.begin=0,.end=4,.layers=4,.context=64,.threads=1,.max_new=24,.ram_bytes=128u<<20,.disk_bytes=128u<<20,.sessions=1};
    lmb_random(o.id,32);memcpy(o.requester,pk,32);memcpy(o.edge_peer,pk,32);memcpy(o.model_root,fixture.checkpoint.root,32);
    strcpy(o.model,fixture.checkpoint.model);strcpy(o.model_type,"olmoe");strcpy(o.tracker,tracker);o.model_bytes=fixture.checkpoint.bytes;
    /* Network membership does not authorize somebody else's peer identity. */
    LmbHomeOffer forged=o;forged.requester[0]^=1;
    LmbSocket fd=lmb_connect_ms_io(endpoint,2000,2000);assert(fd!=LMB_INVALID_SOCKET && !lmb_auth(fd));
    LmbBuf request_body={0};assert(!lmb_home_offer_pack(&request_body,&forged) &&
        !lmb_send(fd,LMB_HOME_OFFER,request_body.p,(uint32_t)request_body.len,NULL,0));free(request_body.p);
    LmbMsg refused={0};assert(lmb_recv_bounded(fd,&refused,4096,2000));lmb_msg_free(&refused);lmb_close(fd);
    assert(!status().has_offer && !atomic_load(&fixture.checkpoint.reads));
    LmbHomeOffer unsupported=o;unsupported.runs_edge=1;unsupported.edge_ram_bytes=32u<<20;
    fd=offer(&unsupported,LMB_HOME_REJECTED);lmb_close(fd);
    LmbHomeOffer declined=o;lmb_random(declined.id,32);fd=offer(&declined,LMB_HOME_PENDING);
    HomeServiceSnapshot pending=status(),decision;
    assert(!home_service_query("donor",HOME_SVC_DECLINE,&pending,&decision));
    assert(receive(fd,&declined)==LMB_HOME_REJECTED && !decision.reserved_total && !atomic_load(&fixture.checkpoint.reads));lmb_close(fd);
    Sleep(30);
    fd=offer(&o,LMB_HOME_PENDING);assert(!atomic_load(&fixture.checkpoint.reads));
    LmbSocket duplicate=offer(&o,LMB_HOME_REJECTED);lmb_close(duplicate);
    approved(fd,&o,1);uint64_t reads=atomic_load(&fixture.checkpoint.reads);assert(reads);
    fixture_chat(&fixture,model,0);
    lmb_close(fd);Sleep(100);HomeServiceSnapshot retained=status();assert(retained.retained && retained.reserved_total==o.ram_bytes);
    WinUi reopened;winui_open(&reopened,binary);winui_expect(reopened.capture,"Weights retained in RAM");winui_close(&reopened);
    HomeServiceSnapshot detached=status();assert(detached.segment_pid==retained.segment_pid && detached.reserved_total==retained.reserved_total);
    const char *restart[]={"service","restart"};assert(!command(restart,2));
    HomeServiceSnapshot after=status();assert(after.pid==retained.pid && after.segment_pid==retained.segment_pid && !memcmp(after.instance,retained.instance,32));
    fixture_chat(&fixture,model,0);assert(atomic_load(&fixture.checkpoint.reads)==reads);
    LmbSegAdvert first=advert_for(&fixture,&o);
    LmbHomeOffer second=o;lmb_random(second.id,32);
    second.runs_edge=1;second.edge_ram_bytes=128u<<20;second.ram_bytes=256u<<20;
    second.context=512;second.max_new=256;second.sessions=2;memcpy(second.edge_peer,reported.identity,32);
    fd=offer(&second,LMB_HOME_PENDING);approved(fd,&second,0);
    assert(!status().host_pid && !host_port); /* Segment readiness does not start Edge. */
    assert(!lmb_send(fd,LMB_HOME_START_HOST,second.id,32,NULL,0));
    unsigned phase;uint64_t start_deadline=GetTickCount64()+30000,pulse=0;
    do {
        assert(GetTickCount64()<start_deadline);
        if (GetTickCount64()>pulse) {assert(!lmb_send(fd,LMB_HOME_PULSE,second.id,32,NULL,0));pulse=GetTickCount64()+1000;}
        phase=receive(fd,&second);
        assert(phase==LMB_HOME_SEGMENT_READY || phase==LMB_HOME_STARTING_HOST || phase==LMB_HOME_READY);
    } while (phase!=LMB_HOME_READY);
    assert(host_port);lmb_close(fd);Sleep(50);
    char host_address[100],host_key[65],host_root[65];
    snprintf(host_address,sizeof host_address,"127.0.0.1:%u",host_port);
    lmb_hex(host_key,reported.identity,32);lmb_hex(host_root,second.model_root,32);
    uint64_t prepared_reads=atomic_load(&fixture.checkpoint.reads);
    LmbSocket host_a=native_host_connect(host_address,0),host_b=native_host_connect(host_address,0);
    LmbSocket host_c=native_host_connect(host_address,1);lmb_close(host_c);
    native_host_send(host_a,"SUBMIT 1 0 2 8 0 1\nhi\n");native_host_completed(host_a,"1",0);
    native_host_send(host_b,"SUBMIT 2 0 2 8 0 1\nhi\n");native_host_completed(host_b,"2",0);
    HomeServiceSnapshot hosted=status();assert(hosted.host_pid && hosted.phase==LMB_HOME_READY && hosted.retained);
    assert(!command(restart,2));
    assert(status().host_pid==hosted.host_pid && status().segment_pid==hosted.segment_pid);
    native_host_send(host_a,"SUBMIT 3 0 2 8 0 1\nhi\n");native_host_completed(host_a,"3",0);
    assert(atomic_load(&fixture.checkpoint.reads)==prepared_reads);
    LmbHostControl hc;
    assert(!lmb_host_control_rpc(host_address,host_key,host_root,LMB_HOST_CONTROL_QUERY,NULL,&hc));
    assert(lmb_host_control_rpc(host_address,host_key,host_root,LMB_HOST_CONTROL_RETIRE,&hc,&hc)>0);
    assert(!lmb_host_control_rpc(host_address,host_key,host_root,LMB_HOST_CONTROL_DRAIN,&hc,&hc));
    assert(lmb_host_control_rpc(host_address,host_key,host_root,LMB_HOST_CONTROL_RETIRE,&hc,&hc)>0);
    lmb_close(host_a);lmb_close(host_b);
    uint64_t idle_deadline=GetTickCount64()+5000;
    do {assert(!lmb_host_control_rpc(host_address,host_key,host_root,LMB_HOST_CONTROL_QUERY,NULL,&hc));
        assert(GetTickCount64()<idle_deadline);Sleep(20);} while (hc.connections);
    assert(!lmb_host_control_rpc(host_address,host_key,host_root,LMB_HOST_CONTROL_RETIRE,&hc,&hc));
    assert(hc.draining==LMB_HOST_CONTROL_RETIRED);
    host_c=native_host_connect(host_address,1);lmb_close(host_c);
    HomeServiceSnapshot two=status();assert(two.model_count==2 && two.reserved_total==o.ram_bytes+second.ram_bytes);

    uint64_t report_deadline=GetTickCount64()+8000;
    do {
        pthread_mutex_lock(&fixture.lock);reported=fixture.machine_report;pthread_mutex_unlock(&fixture.lock);
        if (reported.workload.allocations==2 && reported.workload.reserved_bytes==two.reserved_total) break;
        assert(GetTickCount64()<report_deadline);Sleep(20);
    } while (1);
    assert(!reported.ram_budget_bytes);

    LmbNodeControl current=control(&second,LMB_NODE_CONTROL_QUERY,NULL);
    current=control(&second,LMB_NODE_CONTROL_DRAIN,&current);
    current=control(&second,LMB_NODE_CONTROL_RETIRE,&current);retire(&second,&current);retire(&second,&current);
    after=status();assert(after.reserved_total==o.ram_bytes);
    reads=atomic_load(&fixture.checkpoint.reads);fixture_chat_with_advert(&fixture,model,0,&first);
    assert(atomic_load(&fixture.checkpoint.reads)==reads);
    fd=offer(&second,LMB_HOME_REJECTED);lmb_close(fd);
    uint8_t request[64];memcpy(request,second.id,32);memcpy(request+32,second.model_root,32);
    fd=lmb_connect_ms_io(endpoint,2000,2000);assert(fd!=LMB_INVALID_SOCKET && !lmb_auth(fd));
    assert(!lmb_send(fd,LMB_HOME_EXIT_PROOF,request,sizeof request,NULL,0));LmbMsg reply={0};LmbAllocationLifetime proof;
    assert(!lmb_recv_bounded(fd,&reply,4096,2000) && reply.op==LMB_HOME_EXIT_PROOF_R &&
        !lmb_lifetime_decode(reply.body,reply.body_len,&proof) && proof.reserved_bytes==second.ram_bytes);
    lmb_msg_free(&reply);lmb_close(fd);
    HomeServiceSnapshot selected=status(),unloaded;
    assert(!home_service_query("donor",HOME_SVC_NEXT_MODEL,&selected,&unloaded));selected=status();
    assert(!home_service_query("donor",HOME_SVC_UNLOAD,&selected,&unloaded));
    uint64_t until=GetTickCount64()+5000;
    do {unloaded=status();assert(GetTickCount64()<until);Sleep(10);} while (unloaded.reserved_total);
    const char *stop[]={"service","stop"};assert(!command(stop,2));
    for (unsigned i=0;i<nowners;i++) {assert(!lmb_owned_process_stop(&owners[i],0,5000));assert(!lmb_owned_process_close(&owners[i]));}
    atomic_store(&fixture.stop,1);pthread_join(fixture.thread,NULL);lmb_close(fixture.listener);
    CloseHandle(fixture.registered);pthread_mutex_destroy(&fixture.lock);checkpoint_fixture_free(&fixture.checkpoint);
    puts("NATIVE DONOR PASS: signed inventory, native TUI approval and reattach, approval fences, duplicate refusal, actual resident Edge and Segment, hosted slots/BUSY, 72 oracle tokens, two allocations with independent release, manager restart, retained RAM, terminal receipt and exit proof");
    return 0;
}
