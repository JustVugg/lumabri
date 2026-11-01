/* Actual native launcher, private settings and authenticated tracker probe.
 * No production household or model is used by this test. */
#define NTDDI_VERSION 0x0A000006
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_secure.h"
#include "src/runtime/lumabri_home_settings_windows.h"
#include <assert.h>
#include "console_fixture_windows.h"

typedef struct {LmbSocket listener;HANDLE thread;atomic_int stop;unsigned accepted,rejected;} Tracker;
static DWORD WINAPI tracker_run(void *opaque) {
    Tracker *t=opaque;
    while (!atomic_load(&t->stop)) {
        if (lmb_socket_wait(t->listener,0,20)!=1) continue;
        LmbSocket fd=lmb_socket_accept(t->listener,NULL,NULL);assert(fd!=LMB_INVALID_SOCKET);
        assert(!lmb_socket_timeout(fd,2000) && !lmb_secure_server(fd));
        LmbMsg message={0};int received=lmb_recv_bounded(fd,&message,1024,2000);
        if (received || message.op!=LMB_AUTH || message.pay_len)
            fprintf(stderr,"tracker auth frame: result=%d op=%u body=%u payload=%u error=%d\n",received,message.op,message.body_len,message.pay_len,errno);
        assert(!received && message.op==LMB_AUTH && !message.pay_len);
        LmbCur cursor={message.body,message.body_len,0};char token[128];
        assert(!lmb_cur_str(&cursor,token,sizeof token) && cursor.off==cursor.len);
        int correct=!strcmp(token,"native-household-secret");lmb_msg_free(&message);
        assert(!lmb_send(fd,correct ? LMB_OK : LMB_ERR,NULL,0,NULL,0));
        if (correct) {
            assert(!lmb_recv_bounded(fd,&message,1024,2000) && message.op==LMB_PING && !message.body_len && !message.pay_len);
            lmb_msg_free(&message);assert(!lmb_send(fd,LMB_OK,NULL,0,NULL,0));t->accepted++;
        } else t->rejected++;
        lmb_close(fd);
    }
    return 0;
}
static void wait_settings(const char *tracker) {
    uint64_t until=GetTickCount64()+10000;
    HomeSettings saved;
    for (;;) {
        if (!home_settings_load_windows(&saved) && !strcmp(saved.tracker,tracker) &&
            !strcmp(saved.token,"native-household-secret")) return;
        assert(GetTickCount64()<until);Sleep(10);
    }
}
int wmain(int argc,wchar_t **argv) {
    assert(argc==2);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    wchar_t temporary[512],directory[700];char binary[4096],folder[2100];
    assert(GetTempPathW(512,temporary));
    assert(swprintf(directory,700,L"%lslumabri-home-%lu-%llu-\x00e8",temporary,GetCurrentProcessId(),GetTickCount64())>0);
    assert(CreateDirectoryW(directory,NULL) && SetEnvironmentVariableW(L"LOCALAPPDATA",directory));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[1],-1,binary,sizeof binary,NULL,NULL));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,directory,-1,folder,sizeof folder,NULL,NULL));
    assert(!_putenv_s("LUMABRI_PEER_KEY","") && !_putenv_s("LUMABRI_KNOWN_HOSTS","") &&
        !_putenv_s("LUMABRI_ENCRYPT","1") && !_putenv_s("LUMABRI_HOME_FOREGROUND","") && !lmb_secure_init());
    HomeSettings settings,saved;assert(!home_settings_load_windows(&settings));
    assert(!settings.tracker[0] && !strcmp(settings.ram,"4"));
    char path[1200];assert(!home_settings_path_windows(path,sizeof path));
    HANDLE file=lmb_private_open(path,0,0,NULL);assert(file==INVALID_HANDLE_VALUE && errno==ENOENT);
    /* Parent's tracker identity is fixed; the real launcher uses its own key. */
    char child_key[2200];assert(snprintf(child_key,sizeof child_key,"%s/child.key",folder)>0);
    assert(!_putenv_s("LUMABRI_PEER_KEY",child_key));
    Tracker tracker={0};atomic_init(&tracker.stop,0);
    tracker.listener=lmb_listen(0);assert(tracker.listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in address;int length=sizeof address;
    assert(!getsockname(tracker.listener,(struct sockaddr *)&address,&length));
    char endpoint[128];snprintf(endpoint,sizeof endpoint,"127.0.0.1:%u",ntohs(address.sin_port));
    tracker.thread=CreateThread(NULL,0,tracker_run,&tracker,0,NULL);assert(tracker.thread);
    WinUi ui;winui_open_args(&ui,binary,L"","Join a household");
    winui_send_keys(ui.input,"j\r");winui_expect(ui.capture,"Household address (host:port)");
    winui_send_keys(ui.input,endpoint);winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"Household key from its owner");
    winui_send_keys(ui.input,"wrong-household-secret\r");winui_expect(ui.capture,"Household authentication failed");
    assert(!winui_contains(ui.capture,"wrong-household-secret"));
    assert(lmb_private_open(path,0,0,NULL)==INVALID_HANDLE_VALUE && errno==ENOENT);
    winui_send_keys(ui.input,"\r");Sleep(200); /* return to the menu before closing it */
    winui_close(&ui);
    winui_open_args(&ui,binary,L"","Join a household");
    winui_send_keys(ui.input,"j\r");winui_expect(ui.capture,"Household address (host:port)");
    winui_send_keys(ui.input,endpoint);winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"Household key from its owner");
    winui_send_keys(ui.input,"native-household-secret\r");wait_settings(endpoint);
    assert(!winui_contains(ui.capture,"native-household-secret"));
    winui_close(&ui);
    assert(!home_settings_load_windows(&saved) && !strcmp(saved.tracker,endpoint));
    /* Reopening the real launcher reuses settings, including Unicode paths. */
    winui_open_args(&ui,binary,L"","Join a household");winui_expect(ui.capture,endpoint);
    winui_send_keys(ui.input,"jj\r");winui_expect(ui.capture,"Folder containing your models");
    winui_send_keys(ui.input,"\x15" "C:\\Modèle 日本 😀");winui_expect(ui.capture,"Modèle 日本");
    winui_send_keys(ui.input,"\r");winui_expect(ui.capture,"Maximum RAM to share, in GB");
    winui_send_keys(ui.input,"\x15" "2.5\r");
    uint64_t until=GetTickCount64()+10000;
    do {assert(GetTickCount64()<until);Sleep(10);assert(!home_settings_load_windows(&settings));} while (strcmp(settings.ram,"2.5"));
    assert(!strcmp(settings.models,"C:\\Modèle 日本 😀"));winui_close(&ui);
    winui_open_args(&ui,binary,L"","Join a household");
    winui_send_keys(ui.input,"jj\r");winui_expect(ui.capture,"Folder containing your models");
    winui_send_keys(ui.input,"\x15" "cancelled-path\x1b");Sleep(200);winui_close(&ui);
    assert(!home_settings_load_windows(&saved) && !memcmp(&settings,&saved,sizeof saved));
    saved=settings;strcpy(settings.models,"bad\ntracker=other");
    assert(home_settings_save_windows(&settings));assert(!home_settings_load_windows(&settings));
    assert(!memcmp(&settings,&saved,sizeof settings));
    file=lmb_private_open(path,1,0,NULL);assert(file!=INVALID_HANDLE_VALUE);
    DWORD written;assert(WriteFile(file,"invalid",7,&written,NULL) && written==7 && SetEndOfFile(file));CloseHandle(file);
    assert(home_settings_load_windows(&settings));
    atomic_store(&tracker.stop,1);assert(WaitForSingleObject(tracker.thread,5000)==WAIT_OBJECT_0);
    CloseHandle(tracker.thread);lmb_close(tracker.listener);assert(tracker.accepted==1 && tracker.rejected==1);
    /* Only the generated disposable directory is removed. */
    wchar_t search[800];WIN32_FIND_DATAW entry;
    assert(swprintf(search,800,L"%ls\\Lumabri\\*",directory)>0);
    HANDLE listing=FindFirstFileW(search,&entry);
    if (listing!=INVALID_HANDLE_VALUE) {
        do {if (!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t name[1000];assert(swprintf(name,1000,L"%ls\\Lumabri\\%ls",directory,entry.cFileName)>0 && DeleteFileW(name));
        }} while (FindNextFileW(listing,&entry));FindClose(listing);
    }
    wchar_t sub[800];assert(swprintf(sub,800,L"%ls\\Lumabri\\service",directory)>0);
    DWORD service_attributes=GetFileAttributesW(sub);
    if (service_attributes!=INVALID_FILE_ATTRIBUTES) assert(RemoveDirectoryW(sub));
    assert(swprintf(sub,800,L"%ls\\Lumabri",directory)>0 && RemoveDirectoryW(sub));
    assert(swprintf(sub,800,L"%ls\\child.key",directory)>0 && DeleteFileW(sub));
    assert(RemoveDirectoryW(directory));
    puts("NATIVE HOUSEHOLD UI: PASS (authenticated join, rejected key leaves settings intact, hidden secret, private atomic settings, Unicode input, reopen, invalid data refusal)");
    return 0;
}
