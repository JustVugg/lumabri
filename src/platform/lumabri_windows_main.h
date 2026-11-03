/* Native entrypoint. Add commands here only when their complete path is
 * available; unsupported household operations must not fall back to WSL. */
#ifndef LUMABRI_WINDOWS_MAIN_H
#define LUMABRI_WINDOWS_MAIN_H
#include "src/runtime/lumabri_service.h"
#include "lumabri_machine.h"
#include <signal.h>
#include <stdarg.h>

static volatile sig_atomic_t g_stopping;
static double nowd(void) { return (double)GetTickCount64() / 1000.; }
static char home_error[512];
static int home_fail(const char *format,...) {
    va_list args;va_start(args,format);vsnprintf(home_error,sizeof home_error,format,args);va_end(args);
    fprintf(stderr,"%s\n",home_error);return 1;
}
static void doctor_json_string(const char *value) {
    fputc('"', stdout);
    for (; value && *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else fputc(c, stdout);
    }
    fputc('"', stdout);
}
#include "src/runtime/lumabri_service_manager.h"
#include "src/runtime/lumabri_tracker_windows.h"
#include "src/runtime/lumabri_donor_windows.h"
#include "src/ui/lumabri_donor_windows.h"
#include <math.h>

static int home_donor_entry_windows(int argc,char **argv,int internal) {
    const char *tracker=NULL;uint64_t limit=0;int headless=0;
    for (int i=0;i<argc;i++) {
        if (!strcmp(argv[i],"--join") && i+1<argc) tracker=argv[++i];
        else if (!strcmp(argv[i],"--ram-gb") && i+1<argc) {
            char *end;double value=strtod(argv[++i],&end);
            if (!*argv[i] || *end || !isfinite(value) || value<=0 || value>1048576) return 2;
            limit=(uint64_t)(value*1e9);
        } else if (!strcmp(argv[i],"--headless")) headless=1;
        else return 2;
    }
    if (!tracker || !limit)
        return home_fail("Use donor --join HOST:PORT --ram-gb N. Add --headless only to start without the approval screen.");
    if (internal) {
        if (!getenv("LUMABRI_READY_HANDLE")) return 2;
        wchar_t exe[1024];char worker[1600];DWORD n=GetModuleFileNameW(NULL,exe,1024);
        if (!n || n>=1024) return 1;
        wchar_t *end=wcsrchr(exe,L'\\');if (!end) return 1;
        *end=0;
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,exe,-1,worker,sizeof worker,NULL,NULL) ||
            strlen(worker)+sizeof "/segment_node.exe">sizeof worker) return 1;
        strcat(worker,"/segment_node.exe");
        if (lmb_secure_init() || !lmb_secure_enabled()) return home_fail("Encrypted household authentication is required.");
        LmbWindowsDonor *donor=calloc(1,sizeof *donor);if (!donor) return 1;
        if (lmb_donor_open_windows(donor,worker,tracker,limit)) {free(donor);return home_fail("Cannot start the native donor; no resources were shared.");}
        int rc=lmb_ready_notify();
        while (!rc && !g_stopping && !lmb_donor_step_windows(donor)) Sleep(10);
        lmb_donor_close_windows(donor);free(donor);return rc ? 1 : 0;
    }
    HomeServiceSnapshot old;
    if (!home_service_query("donor",HOME_SVC_STATUS,NULL,&old))
        return strcmp(old.tracker,tracker) ? home_fail("Stop the sharing service before joining another household.") :
            headless ? 0 : lmb_donor_ui_windows(tracker);
    if (home_service_ensure()) return home_fail("Cannot start the household manager.");
    const char *args[32];if (argc>30) return 2;args[0]="--internal-donor";
    for (int i=0;i<argc;i++) args[i+1]=argv[i];
    if (home_service_keeper_start_windows("donor",args,(size_t)argc+1)) return 1;
    return headless ? 0 : lmb_donor_ui_windows(tracker);
}

#include "src/ui/lumabri_chat_windows.h"
#include "src/runtime/lumabri_requester_windows.h"
#include "src/ui/lumabri_home_windows.h"
#include "src/runtime/lumabri_doctor_windows.h"
static int cmd_chat_windows(int argc,char **argv) {
    const char *address=NULL,*key=NULL,*root=NULL,*model=NULL;unsigned max_new=256;
    for(int i=0;i<argc;i++) {
        if(!strcmp(argv[i],"--host") && i+1<argc) address=argv[++i];
        else if(!strcmp(argv[i],"--host-key") && i+1<argc) key=argv[++i];
        else if(!strcmp(argv[i],"--host-root") && i+1<argc) root=argv[++i];
        else if(!strcmp(argv[i],"--model-type") && i+1<argc) model=argv[++i];
        else if(!strcmp(argv[i],"--max-new") && i+1<argc) {
            char *end;unsigned long n=strtoul(argv[++i],&end,10);
            if(!*argv[i] || *end || !n || n>1048576) return 2;
            max_new=(unsigned)n;
        } else return 2;
    }
    uint8_t bytes[32];
    if(!address || !key || !root || strlen(key)!=64 || lmb_unhex(bytes,key,32) ||
        strlen(root)!=64 || lmb_unhex(bytes,root,32))
        return home_fail("Chat requires --host HOST:PORT --host-key PUBLIC_KEY --host-root CHECKPOINT_ROOT from an approved resident plan.");
    HomeSettings settings;
    const char *token=getenv("LUMABRI_TOKEN");
    if(!token || !*token) {
        if(home_settings_load_windows(&settings)) return home_fail("Cannot load private household settings.");
        int bad=home_settings_activate_windows(&settings);SecureZeroMemory(&settings,sizeof settings);
        if(bad) return 1;
    }
    if(_putenv_s("LUMABRI_ENCRYPT","1") || lmb_secure_init() || !lmb_secure_enabled()) return 1;
    return chat_windows_run(address,key,root,model,max_new);
}

static int lmb_windows_main(int argc, char **argv) {
    if (argc>=2 && !strcmp(argv[1],"doctor")) return cmd_doctor_windows(argc-2,argv+2);
    if (argc==1 || (argc==2 && !strcmp(argv[1],"share"))) return home_windows_run(argc==2);
    if (argc>=2 && !strcmp(argv[1],"chat")) return cmd_chat_windows(argc-2,argv+2);
    if (argc>=2 && !strcmp(argv[1],"--internal-prepare")) return home_prepare_entry_windows(argc-2,argv+2);
    if (argc>=2 && !strcmp(argv[1],"--internal-tracker")) return home_tracker_entry_windows(argc-2,argv+2);
    if (argc>=2 && (!strcmp(argv[1],"donor") || !strcmp(argv[1],"--internal-donor")))
        return home_donor_entry_windows(argc-2,argv+2,!strcmp(argv[1],"--internal-donor"));
    if (argc == 2 && !strcmp(argv[1], "--internal-manager"))
        return home_service_manager_entry_windows();
    if (argc >= 2 && !strcmp(argv[1], "service")) {
        if (argc >= 3 && home_service_foreground() &&
            (!strcmp(argv[2], "start") || !strcmp(argv[2], "restart")))
            return home_fail("Remove LUMABRI_HOME_FOREGROUND before starting the native background manager.");
        return cmd_service(argc - 2, argv + 2);
    }
    if (argc == 3 && !strcmp(argv[1], "machine") && !strcmp(argv[2], "--json")) {
        LmbMachineProfile machine;
        if (lmb_machine_probe(&machine, ".", NULL)) return 1;
        lmb_machine_print(stdout, &machine, 1);
        return 0;
    }
    fputs("Lumabri native Windows control\n\n"
          "  lumabri service [start|status|restart|stop] [--json]\n"
          "  lumabri service live ROLE\n"
          "  lumabri machine --json\n\n"
          "  lumabri doctor --household [--json]\n"
          "  lumabri donor --join HOST:PORT --ram-gb N [--headless]\n"
          "  lumabri share  (start or reopen sharing using saved household settings)\n\n"
          "Without arguments: create/join, select computers and models, approve resources and chat.\n"
          "  lumabri chat --host HOST:PORT --host-key KEY --host-root ROOT [--model-type TYPE]\n"
          "Native chat attaches to an approved resident host; it downloads no weights.\n", stderr);
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}

int wmain(int argc, wchar_t **wide) {
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return 1;
    int rc = 1;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!n || !(argv[i] = malloc((size_t)n)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], n, NULL, NULL)) goto done;
    }
    rc = lmb_windows_main(argc, argv);
done:
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv); return rc;
}
#endif
