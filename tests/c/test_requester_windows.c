/* Actual native console -> tracker/source -> approved donor -> resident chat.
 * Every executable is production code; only checkpoint weights are reduced. */
#define NTDDI_VERSION 0x0A000006
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_secure.h"
#include "src/runtime/lumabri_home_settings_windows.h"
#include "src/runtime/lumabri_resident_plan_windows.h"
#include <assert.h>
#include "console_fixture_windows.h"
static char binary[6144];static LmbOwnedProcess commands[12];static unsigned command_count;
static void command(const char *action) {
    assert(command_count<12);const char *args[]={"service",action};
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=2,.error=GetStdHandle(STD_ERROR_HANDLE)};
    LmbOwnedProcess *p=&commands[command_count++];assert(!lmb_owned_process_start(p,&launch));
    assert(WaitForSingleObject(p->process,30000)==WAIT_OBJECT_0);DWORD code;
    assert(GetExitCodeProcess(p->process,&code) && !code);
}
static HomeServiceSnapshot status(const char *role) {
    HomeServiceSnapshot s;uint64_t until=GetTickCount64()+5000;
    while (home_service_query(role,HOME_SVC_STATUS,NULL,&s)) {assert(GetTickCount64()<until);Sleep(20);}
    return s;
}
static void copy_fixture(const wchar_t *from,const wchar_t *to) {
    assert(CreateDirectoryW(to,NULL));wchar_t pattern[1800];assert(swprintf(pattern,1800,L"%ls\\*",from)>0);
    WIN32_FIND_DATAW e;HANDLE scan=FindFirstFileW(pattern,&e);assert(scan!=INVALID_HANDLE_VALUE);
    do {
        if (e.cFileName[0]==L'.') continue;
        wchar_t a[1800],b[1800];assert(swprintf(a,1800,L"%ls\\%ls",from,e.cFileName)>0 && swprintf(b,1800,L"%ls\\%ls",to,e.cFileName)>0);
        assert(!(e.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));
        if (e.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) copy_fixture(a,b);
        else assert(CopyFileW(a,b,TRUE));
    } while (FindNextFileW(scan,&e));FindClose(scan);
}
static void remove_fixture(const wchar_t *dir) {
    wchar_t pattern[1800];assert(swprintf(pattern,1800,L"%ls\\*",dir)>0);
    WIN32_FIND_DATAW e;HANDLE scan=FindFirstFileW(pattern,&e);assert(scan!=INVALID_HANDLE_VALUE);
    do {
        if (!wcscmp(e.cFileName,L".") || !wcscmp(e.cFileName,L"..")) continue;
        assert(!(e.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));wchar_t path[1800];assert(swprintf(path,1800,L"%ls\\%ls",dir,e.cFileName)>0);
        if (e.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) remove_fixture(path);else assert(DeleteFileW(path));
    } while (FindNextFileW(scan,&e));FindClose(scan);assert(RemoveDirectoryW(dir));
}
static void open_catalogue(WinUi *ui) {
    winui_open_args(ui,binary,L"","Create a household");winui_send_keys(ui->input,"jjjjjj\r");
    winui_expect(ui->capture,"Models");Sleep(1500);
    winui_send_keys(ui->input,"\t");Sleep(700);winui_send_keys(ui->input," ");Sleep(1000);
    winui_send_keys(ui->input,"\t\r");winui_expect(ui->capture,"Enter requests this plan");
}
/* Installed packages are switched only after an explicit stop. Keep process
 * handles, not reusable PID numbers, when verifying old workers have exited. */
static void stopped_workers(void) {
    HomeServiceSnapshot donor=status("donor");HANDLE workers[3];unsigned count=0;
    DWORD pids[]={donor.pid,donor.segment_pid,donor.host_pid};
    for (unsigned j=0;j<3;j++) if (pids[j]) {
        workers[count]=OpenProcess(SYNCHRONIZE,FALSE,pids[j]);assert(workers[count]);count++;
    }
    command("stop");
    for (unsigned i=0;i<count;i++) {assert(WaitForSingleObject(workers[i],5000)==WAIT_OBJECT_0);CloseHandle(workers[i]);}
    HomeServiceSnapshot absent;assert(home_service_query("donor",HOME_SVC_STATUS,NULL,&absent));
    assert(home_service_query("tracker",HOME_SVC_STATUS,NULL,&absent));
}
static void switch_package(const wchar_t *next,const HomeSettings *original,const uint8_t prior[32]) {
    stopped_workers();
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,next,-1,binary,sizeof binary,NULL,NULL));
    HomeSettings settings;assert(!home_settings_load_windows(&settings));
    assert(!strcmp(settings.tracker,original->tracker) && !strcmp(settings.token,original->token) &&
        !strcmp(settings.models,original->models) && settings.owner);
    WinUi ui;winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjj\r");
    winui_expect(ui.capture,"Connect your other computers");winui_send_keys(ui.input,"\r");Sleep(150);winui_close(&ui);
    open_catalogue(&ui);winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"This computer needs your approval too.");
    HomeServiceSnapshot pending=status("donor");assert(!pending.reserved_total && pending.has_offer &&
        memcmp(pending.offer.id,prior,32));
    /* Changed packages never revive an old approval. */
    winui_send_keys(ui.input,"a");winui_expect(ui.capture,"conversation");
    winui_send_keys(ui.input,"Hello after the package switch.\r");winui_expect(ui.capture,"Reply 1");
    winui_send_keys(ui.input,"/quit\r");Sleep(200);winui_send_keys(ui.input,"q");Sleep(200);winui_close(&ui);
    assert(status("donor").reserved_total>0);
}
int wmain(int argc,wchar_t **argv) {
    assert(argc==3 || argc==4);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    wchar_t temp[512],directory[700],models[900],model[1000];assert(GetTempPathW(512,temp));
    assert(swprintf(directory,700,L"%lslumabri-requester-%lu-%llu-\x00e8",temp,GetCurrentProcessId(),GetTickCount64())>0);
    assert(CreateDirectoryW(directory,NULL) && SetEnvironmentVariableW(L"LOCALAPPDATA",directory));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[1],-1,binary,sizeof binary,NULL,NULL));
    char profile[2100];assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,directory,-1,profile,sizeof profile,NULL,NULL));
    fprintf(stderr,"Test profile: %s\n",profile);fflush(stderr);
    assert(swprintf(models,900,L"%ls\\models",directory)>0 && CreateDirectoryW(models,NULL));
    assert(swprintf(model,1000,L"%ls\\tiny",models)>0);copy_fixture(argv[2],model);
    assert(swprintf(model,1000,L"%ls\\second",models)>0);copy_fixture(argv[2],model);
    assert(!_putenv_s("LUMABRI_ADVERTISE","127.0.0.1") && !_putenv_s("LUMABRI_ENCRYPT","1") &&
        !_putenv_s("LUMABRI_PEER_KEY","") && !_putenv_s("LUMABRI_KNOWN_HOSTS","") &&
        !_putenv_s("LUMABRI_HOME_FOREGROUND","") && !_putenv_s("LUMABRI_TOKEN","") &&
        !_putenv_s("LUMABRI_HOME_HYBRID","0") && !_putenv_s("LUMABRI_RAM_RESERVE_MB","256") && !lmb_secure_init());
    LmbSocket sock=lmb_listen(0);assert(sock!=LMB_INVALID_SOCKET);struct sockaddr_in a;int alen=sizeof a;
    assert(!getsockname(sock,(struct sockaddr *)&a,&alen));char port[16];snprintf(port,sizeof port,"%u",ntohs(a.sin_port));lmb_close(sock);
    assert(!_putenv_s("LUMABRI_HOME_PORT_BASE",port));
    WinUi ui;winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjj\r");
    winui_expect(ui.capture,"Connect your other computers");winui_send_keys(ui.input,"\r");Sleep(150);winui_close(&ui);
    HomeSettings settings;assert(!home_settings_load_windows(&settings) && settings.owner);
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,models,-1,settings.models,sizeof settings.models,NULL,NULL));
    strcpy(settings.context,"512");strcpy(settings.sessions,"2");strcpy(settings.ram,"1.5");
    assert(!home_settings_save_windows(&settings) && !home_settings_activate_windows(&settings));
    /* Explicit donor selection, review, request, then local approval through
     * the same native preparation screen a person sees. */
    open_catalogue(&ui);winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"This computer needs your approval too.");
    HomeServiceSnapshot pending=status("donor");assert(pending.phase==LMB_HOME_PENDING && !pending.reserved_total);
    HomeServiceSnapshot prepare=status("prepare");assert(prepare.preparation_result_count==1 &&
        !memcmp(prepare.preparation_results[0].allocation,pending.offer.id,32));
    /* Closing the console does not cancel the pending operation or replay it. */
    winui_send_keys(ui.input,"\x1b");Sleep(300);winui_send_keys(ui.input,"q");Sleep(300);
    winui_close(&ui);Sleep(300);
    HomeServiceSnapshot still=status("prepare");assert(!memcmp(still.instance,prepare.instance,32));
    winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjjjj\r");
    winui_expect(ui.capture,"This computer needs your approval too.");
    pending=status("donor");assert(!pending.reserved_total);
    winui_send_keys(ui.input,"a");winui_expect(ui.capture,"conversation");
    HomeServiceSnapshot loaded=status("donor");assert(loaded.retained && loaded.reserved_total && loaded.segment_pid && loaded.host_pid);
    winui_send_keys(ui.input,"Hello from the native catalogue.\r");winui_expect(ui.capture,"Reply 1");
    winui_send_keys(ui.input,"/quit\r");Sleep(200);winui_send_keys(ui.input,"q");Sleep(200);winui_close(&ui);
    HomeServiceSnapshot detached=status("donor");assert(detached.segment_pid==loaded.segment_pid && detached.reserved_total==loaded.reserved_total);
    command("restart");HomeServiceSnapshot restarted=status("donor");assert(!memcmp(restarted.instance,loaded.instance,32));
    LmbResidentPlan saved[4];size_t count=home_resident_library_list(settings.tracker,saved,4);assert(count==1 && saved[0].sessions==2);
    assert(!memcmp(saved[0].allocation,pending.offer.id,32));
    winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjjjjj\r");
    winui_expect(ui.capture,"Continue with an approved model");winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"conversation");
    winui_send_keys(ui.input,"A new private conversation.\r");winui_expect(ui.capture,"Reply 1");
    assert(!winui_contains(ui.capture,"Hello from the native catalogue."));
    winui_send_keys(ui.input,"/quit\r");Sleep(200);winui_close(&ui);
    /* A short real probe reuses the resident model and persists no text. */
    Sleep(5500);
    winui_open_args(&ui,binary,L"","Create a household");winui_send_keys(ui.input,"jjjjjjj\r");
    winui_expect(ui.capture,"Continue with an approved model");winui_send_keys(ui.input,"c");
    winui_expect(ui.capture,"Measurement saved");
    char records[1200],record_path[1400];uint8_t bytes[LMB_CAL_RECORD_MAX];size_t length;LmbCalibration measurement;
    assert(!lmb_private_default_path(records,sizeof records,"calibrations"));
    snprintf(record_path,sizeof record_path,"%s\\%s.cal",records,saved[0].content_id);
    assert(!lmb_private_blob_read(record_path,bytes,sizeof bytes,&length) && !lmb_cal_decode(bytes,length,&measurement));
    assert(measurement.source==LMB_CAL_SOURCE_PROBE && measurement.generated_tokens<=8 && measurement.samples>=1);
    assert(!strcmp(measurement.key.build_id,saved[0].observation.build_id));
    winui_send_keys(ui.input,"\r");Sleep(200);winui_close(&ui);
    assert(status("donor").reserved_total==loaded.reserved_total);
    /* Refusal rolls back only the new request; the earlier model remains. */
    open_catalogue(&ui);winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"This computer needs your approval too.");
    winui_send_keys(ui.input,"d");winui_expect(ui.capture,"Preparation did not complete");
    winui_send_keys(ui.input,"\r");Sleep(200);winui_send_keys(ui.input,"q");Sleep(200);winui_close(&ui);
    HomeServiceSnapshot after=status("donor");assert(after.reserved_total==loaded.reserved_total);
    count=home_resident_library_list(settings.tracker,saved,4);assert(count==1);
    /* Cancellation also preserves the original READY allocation. */
    open_catalogue(&ui);winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"This computer needs your approval too.");
    winui_send_keys(ui.input,"x");winui_expect(ui.capture,"Preparation did not complete");
    winui_send_keys(ui.input,"\r");Sleep(200);winui_send_keys(ui.input,"q");Sleep(200);winui_close(&ui);
    assert(status("donor").reserved_total==loaded.reserved_total);
    /* Joint review has one operation and distinct per-model approvals. */
    open_catalogue(&ui);winui_send_keys(ui.input,"\x1b");Sleep(200);
    winui_send_keys(ui.input," ");Sleep(1200);winui_send_keys(ui.input,"j ");Sleep(1200);
    winui_send_keys(ui.input,"/jjjjj\r");winui_expect(ui.capture,"Enter prepares this joint plan");
    winui_send_keys(ui.input,"\r");uint8_t prior[32]={0};unsigned accepted=0;
    uint64_t until=GetTickCount64()+60000;
    while (accepted<2) {
        assert(GetTickCount64()<until);HomeServiceSnapshot donor=status("donor");
        if (donor.phase==LMB_HOME_PENDING && donor.has_offer && memcmp(donor.offer.id,prior,32)) {
            HomeServiceSnapshot operation=status("prepare"),reply;int own=0;
            for (uint32_t i=0;i<operation.preparation_result_count;i++)
                if (!memcmp(operation.preparation_results[i].allocation,donor.offer.id,32)) own=1;
            assert(own && !home_service_query("donor",HOME_SVC_ACCEPT,&donor,&reply));
            memcpy(prior,donor.offer.id,32);accepted++;
        }
        Sleep(50);
    }
    winui_expect(ui.capture,"Selected models ready");
    assert(!home_service_record("prepare",&prepare) && prepare.state==HOME_SVC_DONE && prepare.preparation_result_count==2);
    for (unsigned i=0;i<2;i++) assert(prepare.preparation_results[i].ready_observed);
    assert(home_resident_library_list(settings.tracker,saved,4)==3 && status("donor").reserved_total>loaded.reserved_total);
    winui_send_keys(ui.input,"\r");Sleep(200);winui_send_keys(ui.input,"q");Sleep(200);winui_close(&ui);
    if (argc==4) {
        uint8_t old_id[32];memcpy(old_id,pending.offer.id,32);
        switch_package(argv[3],&settings,old_id);
        HomeServiceSnapshot current=status("donor");memcpy(old_id,current.offer.id,32);
        switch_package(argv[1],&settings,old_id);
        puts("WINDOWS PACKAGE SWITCH: PASS (quiescent update and rollback, worker exit, persistent identity/settings, no approval replay, resident chat after each switch)");
    }
    stopped_workers();
    for (unsigned i=0;i<command_count;i++) assert(!lmb_owned_process_stop(&commands[i],0,5000) && !lmb_owned_process_close(&commands[i]));
    remove_fixture(directory);
    puts("WINDOWS REQUESTER: PASS (production catalogue, explicit selection, exact source, local approval, resident chat, detach/reopen, manager restart, private conversation, 8-token calibration, cancellation and two-model joint preparation)");return 0;
}
