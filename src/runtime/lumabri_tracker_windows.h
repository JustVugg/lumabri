/* Native service owner for the common tracker executable. A console owns
 * startup until READY; the keeper then owns the tracker and reserved port. */
#ifndef LMB_TRACKER_WINDOWS_H
#define LMB_TRACKER_WINDOWS_H
#include "lumabri_home_net.h"
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_ready.h"

static int home_tracker_entry_windows(int argc,char **argv) {
    if (argc!=4 || strcmp(argv[0],"--port") || strcmp(argv[2],"--address") || !getenv("LUMABRI_READY_HANDLE")) return 2;
    char *end;long port=strtol(argv[1],&end,10);
    if (!*argv[1] || *end || port<1024 || port>65535 || strlen(argv[3])>=256) return 2;
    HomeService service;
    if (home_service_open(&service,"tracker")) return errno==EBUSY ? 75 : 1;
    LmbOwnedProcess child={0};LmbSocket listener=LMB_INVALID_SOCKET;
    HANDLE pair[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE},ready=NULL;
    int rc=1;wchar_t wide[2048];char binary[6144];
    DWORD n=GetModuleFileNameW(NULL,wide,2048);
    if (!n || n>=2048) goto done;
    wchar_t *slash=wcsrchr(wide,L'\\');if (!slash || (size_t)(slash-wide)+sizeof L"\\tracker.exe"/sizeof(wchar_t)>2048) goto done;
    wcscpy(slash,L"\\tracker.exe");
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,binary,sizeof binary,NULL,NULL)) goto done;
    listener=lmb_listen((int)port);
    if (listener==LMB_INVALID_SOCKET || lmb_pipe_pair(pair) || !(ready=CreateEventW(NULL,TRUE,FALSE,NULL))) goto done;
    LmbChildHandle handles[]={{"LUMABRI_HOME_LISTEN_HANDLE",pair[1]},{"LUMABRI_READY_HANDLE",ready}};
    const char *args[]={"--port",argv[1],"--household-discovery"};
    LmbProcessLaunch launch={.binary=binary,.args=args,.nargs=3,.handles=handles,.nhandles=2,
        .output=GetStdHandle(STD_OUTPUT_HANDLE),.error=GetStdHandle(STD_ERROR_HANDLE)};
    if (lmb_owned_process_start(&child,&launch)) goto done;
    CloseHandle(pair[1]);pair[1]=INVALID_HANDLE_VALUE;
    if (lmb_home_export_listener(listener,child.pid,pair[0])) goto done;
    HANDLE waits[]={ready,child.process};
    if (WaitForMultipleObjects(2,waits,FALSE,25000)!=WAIT_OBJECT_0) goto done;
    snprintf(service.snapshot.name,sizeof service.snapshot.name,"Household index");
    snprintf(service.snapshot.tracker,sizeof service.snapshot.tracker,"%s",argv[3]);
    snprintf(service.snapshot.detail,sizeof service.snapshot.detail,"Authenticated household index is ready. Closing the workspace keeps it running.");
    if (home_service_save(&service) || lmb_ready_notify()) goto done;
    rc=0;
    while (!g_stopping) {
        if (WaitForSingleObject(child.process,0)!=WAIT_TIMEOUT) {rc=1;break;}
        if (home_service_save(&service)) {rc=1;break;}
        int op=home_service_poll(&service);
        if (op==HOME_SVC_STOP) break;
        home_service_answer(&service);Sleep(50);
    }
done:
    if (child.process) {
        if (lmb_owned_process_stop(&child,0,5000) || lmb_owned_process_close(&child)) {
            rc=1;CloseHandle(child.process);CloseHandle(child.job);
        }
    }
    if (listener!=LMB_INVALID_SOCKET) lmb_socket_close(listener);
    if (ready) CloseHandle(ready);
    for (unsigned i=0;i<2;i++) if (pair[i]!=INVALID_HANDLE_VALUE) CloseHandle(pair[i]);
    service.snapshot.state=rc ? HOME_SVC_FAILED : HOME_SVC_STOPPED;
    snprintf(service.snapshot.detail,sizeof service.snapshot.detail,"%s",rc ?
        "The household index did not start or exited. Check its service log; no replacement was started." : "Household index stopped.");
    if (home_service_save(&service)) rc=1;
    home_service_answer(&service);home_service_close(&service);return rc;
}
#endif
