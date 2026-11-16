/* `lumabri start` on native Windows: the household, sharing and the loopback
 * page, each through the keeper the console workspace uses, and a live keeper
 * is adopted rather than replaced. Included by lumabri_start.h, which holds
 * the sign-in token rules shared with POSIX. */
#ifndef LUMABRI_START_WINDOWS_H
#define LUMABRI_START_WINDOWS_H

/* Inference grants name exact approved allocations. Give the local identity
 * every plan this household has approved so far. Returns how many are still
 * not granted. */
static unsigned start_grant_approved(int dir, const char *tracker, unsigned *granted) {
    LmbResidentPlan *plans=calloc(64,sizeof *plans); unsigned missing=0; *granted=0;
    size_t count=plans ? home_resident_library_list(tracker,plans,64) : 0;
    for (size_t i=0; i<count; i++) {
        LmbApiUser user;
        if (!lmb_home_nonzero(plans[i].allocation,32)) continue;
        if (lmb_api_user_load(dir,LMB_START_USER,&user)) { missing++; continue; }
        if (lmb_api_user_allows(&user,plans[i].allocation)) continue;
        int lock=api_access_wait_lock(dir);
        int rc=lock<0 ? -1 : api_grant_allocation(dir,tracker,LMB_START_USER,plans[i].allocation);
        if (lock>=0) close(lock);
        if (rc) missing++; else (*granted)++;
    }
    free(plans); return missing;
}
/* "Share resources" without its screen. It only publishes capacity: loading a
 * model still needs the approval of this computer's owner. */
static int start_sharing_windows(HomeSettings *s, char *notice, size_t cap) {
    HomeServiceSnapshot seen;
    if (!home_service_query("donor",HOME_SVC_STATUS,NULL,&seen) && seen.state==HOME_SVC_RUNNING) {
        if (!strcmp(seen.tracker,s->tracker)) return 0;
        snprintf(notice,cap,"This computer is already sharing with another household (%.80s). It was left as it is.",seen.tracker);
        return 1;
    }
    char *args[]={"--join",s->tracker,"--ram-gb",s->ram,"--headless"};
    if (home_donor_entry_windows(5,args,0)) {
        snprintf(notice,cap,"Sharing did not start. Check that segment_node.exe is installed beside lumabri.exe.");
        return 1;
    }
    return 0;
}
/* The fragment is the only place the token appears; it is handed to the
 * registered browser as one argument, with no shell parsing, and the
 * household key is removed from the environment the browser inherits. */
static int start_open_browser_windows(const char *url) {
    wchar_t wide[256];
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,url,-1,wide,sizeof wide/sizeof *wide)) return -1;
    (void)_putenv_s("LUMABRI_TOKEN","");
    INT_PTR rc=(INT_PTR)ShellExecuteW(NULL,L"open",wide,NULL,NULL,SW_SHOWNORMAL);
    SecureZeroMemory(wide,sizeof wide);
    return rc>32 ? 0 : -1;
}
static int cmd_start_windows(int argc, char **argv) {
    int tui=0, browser=1, port_set=0; unsigned port=47380; unsigned long share_gb=0;
    for (int i=0; i<argc; i++) {
        if (!strcmp(argv[i],"--tui")) tui=1;
        else if (!strcmp(argv[i],"--no-browser")) browser=0;
        else if (!strcmp(argv[i],"--port") && i+1<argc) {
            char *end; unsigned long value=strtoul(argv[++i],&end,10);
            if (!*argv[i] || *end || !value || value>65535) goto usage;
            port=(unsigned)value; port_set=1;
        } else if (!strcmp(argv[i],"--ram-gb") && i+1<argc) {
            char *end; share_gb=strtoul(argv[++i],&end,10);
            if (!*argv[i] || *end || !share_gb || share_gb>1048576) goto usage;
        } else goto usage;
    }
    if (home_service_foreground())
        return home_fail("lumabri start uses the background service. Remove LUMABRI_HOME_FOREGROUND and retry.");
    HomeSettings s;
    if (home_settings_load_windows(&s))
        return home_fail("The private settings file is unreadable or invalid. It has not been overwritten.");
    if (_putenv_s("LUMABRI_ENCRYPT","1") || lmb_secure_init() || !lmb_secure_enabled() || home_settings_activate_windows(&s))
        return home_fail("Cannot activate encrypted household access.");
    char notice[400]=""; int rc=1;
    const char *household="joined earlier";
    if (s.owner || !s.tracker[0]) {
        int fresh=!s.tracker[0];
        /* The built-in 4 GB share is smaller than any model worth a first
         * chat. A household created here starts by offering about 60% of
         * this computer's memory; the donor still caps every reservation at
         * what is free minus its reserve, and Settings changes the figure. */
        if (fresh && (share_gb || !strcmp(s.ram,"4"))) {
            HomeSettings next=s;
            if (share_gb) snprintf(next.ram,sizeof next.ram,"%lu",share_gb);
            else {
                LmbMachineProfile machine;
                (void)lmb_machine_probe(&machine,".",NULL);
                uint64_t share=machine.ram_total_bytes/UINT64_C(1000000000)*6/10;
                if (share>4 && share<=1048576) snprintf(next.ram,sizeof next.ram,"%llu",(unsigned long long)share);
            }
            if (strcmp(next.ram,s.ram)) {
                if (home_settings_save_windows(&next)) { SecureZeroMemory(&next,sizeof next); home_fail("Cannot save the household settings."); goto done; }
                s=next;
            }
            SecureZeroMemory(&next,sizeof next);
        }
        int created=home_create_windows(&s);
        if (created) {
            home_fail(created==-4 ? "Local services or settings changed. Stop sharing before creating another household, then start again." :
                created==-3 ? "Settings changed in another workspace or could not be saved. The new index was stopped." :
                created==-6 ? "Startup was not confirmed and shutdown could not be verified. Check lumabri service status before retrying." :
                "The household could not be started. Check that tracker.exe is installed beside lumabri.exe, the LAN address and the household port.");
            goto done;
        }
        household=fresh ? "created now" : "yours";
    } else if (!s.token[0] || home_connection_check_windows(s.tracker)) {
        home_fail("This computer joined the household at %.200s, which is not reachable now. "
            "Start Lumabri on that computer first, or use lumabri start --tui to join another.",s.tracker);
        goto done;
    }
    /* A first start should not point the page at a folder that is absent. */
    char default_models[512];
    if (!lmb_private_default_path(default_models,sizeof default_models,"models") && !strcmp(default_models,s.models))
        (void)lmb_private_mkdir(s.models);
    int degraded=start_sharing_windows(&s,notice,sizeof notice);
    if (degraded) fprintf(stderr,"%s\n",notice);

    HomeServiceSnapshot api;
    if (!home_service_query("api",HOME_SVC_STATUS,NULL,&api)) {
        /* Reuse the page that is already being served instead of asking for
         * a second gateway on another port. */
        unsigned live=0; char tail=0;
        if (sscanf(api.name,"http://127.0.0.1:%u%c",&live,&tail)!=1 || !live || live>65535) {
            home_fail("The web gateway is running with another configuration. Run lumabri api stop first."); goto done;
        }
        if (!port_set) port=live;
    }
    if (api_start_windows(port,s.tracker)) { home_fail("The web page could not be started on 127.0.0.1:%u.",port); goto done; }
    int dir=api_access_open_windows();
    if (dir<0) goto done;
    char token[98]={0}; int created=0, taken=start_local_user(dir,token,&created);
    if (taken<0) { close(dir); home_fail("Cannot prepare the local web sign-in. The API access database is busy or unsafe."); goto done; }
    unsigned granted=0, missing=taken ? 0 : start_grant_approved(dir,s.tracker,&granted);
    close(dir);

    char address[64], link[200]="";
    snprintf(address,sizeof address,"http://127.0.0.1:%u/",port);
    if (!taken) snprintf(link,sizeof link,"%s#token=%s",address,token);
    lmb_api_wipe(token,sizeof token);
    printf("Lumabri is running on this computer.\n"
           "  Chat page:      %s\n"
           "  Household:      %s (%s)\n"
           "  Sharing:        %s%s%s\n"
           "  Models folder:  %s\n"
           "  Stop:           lumabri service stop\n",
        address,s.tracker,household,degraded ? "not active" : "up to ",degraded ? "" : s.ram,
        degraded ? "" : " GB of this computer's memory",s.models);
    if (granted) printf("  %u approved model%s added to the local sign-in.\n",granted,granted==1 ? " was" : "s were");
    if (missing) printf("  %u approved model%s could not be added to the local sign-in.\n",missing,missing==1 ? "" : "s");
    if (taken)
        printf("\nA web user named \"" LMB_START_USER "\" already exists and its token is not saved on this computer.\n"
               "Sign in on the page with that token, or run: lumabri api revoke " LMB_START_USER "\nand start again.\n");
    fflush(stdout);
    rc=degraded || missing ? 1 : 0;
    if (tui) { lmb_api_wipe(link,sizeof link); SecureZeroMemory(&s,sizeof s); return home_windows_run(0); }
    int opened=browser && !start_open_browser_windows(taken ? address : link);
    if (browser && !opened) fprintf(stderr,"\nNo browser could be opened from here.\n");
    if (!opened && !taken)
        printf("\nOpen this private link in a browser on this computer. It signs you in; do not share it.\n  %s\n",link);
    lmb_api_wipe(link,sizeof link);
done:
    SecureZeroMemory(&s,sizeof s); return rc;
usage:
    fprintf(stderr,"lumabri start [--tui] [--no-browser] [--port N] [--ram-gb N]\n"
        "  Creates this computer's household if there is none, starts sharing and the\n"
        "  local web page, then opens that page in the default browser.\n"
        "  --tui         open the console workspace instead of the browser\n"
        "  --no-browser  print the private sign-in link instead of opening it\n"
        "  --port N      local web page port (default 47380, or the running page)\n"
        "  --ram-gb N    memory a new household shares (default: about 60%% of this computer)\n");
    return 2;
}
#endif
