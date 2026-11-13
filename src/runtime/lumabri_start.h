/* One-step local start: household, sharing and the loopback web page.
 * Every piece is started through the same keeper the TUI or the api command
 * uses, and a live keeper is adopted rather than replaced. Nothing here widens
 * what the gateway serves: it stays on 127.0.0.1, and the page still needs a
 * bearer token. That token reaches the browser only in a URL fragment, which
 * is never sent to a server and is never written to a log. */
#ifndef LUMABRI_START_H
#define LUMABRI_START_H

#define LMB_START_USER "local"
#define LMB_START_TOKEN_FILE LMB_START_USER ".token"

/* The gateway persists only token digests, so reopening the page later needs
 * one private copy of this computer's own token. It lives beside the digests
 * in the owner-only access directory, with the same anchored, no-follow,
 * owner-only rules as every other record there. */
static int start_token_read(int dir, char token[98]) {
    int fd=openat(dir,LMB_START_TOKEN_FILE,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return -1;
    struct stat st; char bytes[98]={0};
    int bad=lmb_api_private_fd(fd,0) || fstat(fd,&st) || st.st_size<=0 || (size_t)st.st_size>=sizeof bytes ||
        lmb_read_full(fd,bytes,(size_t)st.st_size);
    close(fd);
    if (!bad) memcpy(token,bytes,sizeof bytes);
    lmb_api_wipe(bytes,sizeof bytes);
    return bad ? -1 : 0;
}
/* Caller holds the access database lock. */
static int start_token_write(int dir, const char *token) {
    static const char temporary[]=".start-token.pending";
    int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if (fd<0) return -1;
    int bad=lmb_api_private_fd(fd,0) || ftruncate(fd,0); size_t at=0,n=strlen(token);
    while (!bad && at<n) {
        ssize_t wrote=write(fd,token+at,n-at);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) bad=1; else at+=(size_t)wrote;
    }
    if (!bad && fsync(fd)) bad=1;
    if (close(fd)) bad=1;
    if (!bad && renameat(dir,temporary,dir,LMB_START_TOKEN_FILE)) bad=1;
    if (bad) (void)unlinkat(dir,temporary,0);
    else if (fsync(dir)) bad=1;
    return bad ? -1 : 0;
}
static int start_token_valid(int dir, const char *token) {
    char header[128]; LmbApiUser user;
    int ok=!checked_printf(header,sizeof header,"Bearer %s",token) && !lmb_api_authorize(dir,header,&user) &&
        !strcmp(user.name,LMB_START_USER);
    lmb_api_wipe(header,sizeof header); lmb_api_wipe(&user,sizeof user);
    return ok;
}
/* 0: token is usable. 1: the name is taken by a credential this command did
 * not save, which is never deleted or replaced here. -1: storage failure.
 * Management is granted once, when the identity is created, so that the
 * person at this computer can prepare a first model from the page. A later
 * explicit `api manage-revoke` is respected and not undone. */
static int start_local_user(int dir, char token[98], int *created) {
    *created=0;
    int lock=-1;
    for (unsigned attempt=0; attempt<40 && lock<0; attempt++) {
        lock=lmb_api_access_lock(dir);
        if (lock<0) { if (errno!=EAGAIN && errno!=EWOULDBLOCK) return -1; (void)poll(NULL,0,50); }
    }
    if (lock<0) return -1;
    int rc=-1; LmbApiUser user;
    if (!start_token_read(dir,token) && start_token_valid(dir,token)) rc=0;
    else if (!lmb_api_user_load(dir,LMB_START_USER,&user)) rc=1;
    else if (!lmb_api_user_create(dir,LMB_START_USER,token)) {
        /* An identity whose token cannot be handed over is useless: remove
         * it so that the next start can try again from a clean state. */
        if (start_token_write(dir,token) || lmb_api_user_load(dir,LMB_START_USER,&user) ||
            lmb_management_set(dir,&user,1)) {
            (void)unlinkat(dir,LMB_START_USER ".user",0); (void)unlinkat(dir,LMB_START_TOKEN_FILE,0);
            (void)unlinkat(dir,LMB_START_USER ".manager",0); (void)fsync(dir);
        } else { rc=0; *created=1; }
    }
    if (rc) lmb_api_wipe(token,98);
    lmb_api_wipe(&user,sizeof user);
    close(lock); return rc;
}
/* Inference grants name exact approved allocations; there is no wildcard and
 * none is invented here. Give the local identity every plan and managed model
 * this household has approved so far, through the ordinary grant command.
 * Returns how many are still not granted. */
static unsigned start_grant_approved(int dir, const char *tracker, unsigned *granted) {
    uint8_t (*wanted)[32]=calloc(64+LMB_ROUTE_MAX,32);
    LmbResidentPlan *plans=calloc(64,sizeof *plans);
    LmbRouteRegistry *routes=calloc(1,sizeof *routes);
    unsigned count=0,missing=0; *granted=0;
    if (wanted && plans && routes) {
        size_t n=home_resident_library_list(tracker,plans,64);
        for (size_t i=0; i<n; i++)
            if (lmb_home_nonzero(plans[i].allocation,32)) memcpy(wanted[count++],plans[i].allocation,32);
        int route_dir=lmb_route_dir(dir,0);
        if (route_dir>=0) {
            if (!lmb_route_registry_read(route_dir,routes))
                for (uint32_t i=0; i<routes->count; i++)
                    if (!strcmp(routes->routes[i].tracker,tracker)) memcpy(wanted[count++],routes->routes[i].id,32);
            close(route_dir);
        }
    }
    for (unsigned i=0; i<count; i++) {
        LmbApiUser user;
        if (lmb_api_user_load(dir,LMB_START_USER,&user)) { missing+=count-i; break; }
        if (lmb_api_user_allows(&user,wanted[i])) continue;
        char id[65]; lmb_hex(id,wanted[i],32);
        char *grant[]={"grant",LMB_START_USER,id};
        if (user.count>=LMB_API_MODELS_MAX || cmd_api(3,grant)) missing++; else (*granted)++;
    }
    free(wanted); free(plans); free(routes);
    return missing;
}

static int start_donor_live(HomeServiceSnapshot *seen) {
    return !home_service_query("donor",HOME_SVC_STATUS,NULL,seen) && seen->state==HOME_SVC_RUNNING;
}
/* "Share resources" without its screen: the supervised headless donor, in
 * its own session so that closing the launching window does not stop it.
 * It only publishes capacity. Loading a model still needs the approval of
 * this computer's owner. 0: sharing with this household. 1: not sharing. */
static int start_sharing(const HomeSettings *s, char *notice, size_t cap) {
    HomeServiceSnapshot seen;
    if (start_donor_live(&seen)) {
        if (!strcmp(seen.tracker,s->tracker)) return 0;
        snprintf(notice,cap,"This computer is already sharing with another household (%.80s). It was left as it is.",seen.tracker);
        return 1;
    }
    char dir[1024], binary[1200], log[1200];
    exe_dir(dir,sizeof dir);
    if (home_service_ensure() || checked_printf(binary,sizeof binary,"%s/lumabri",dir) ||
        home_service_path("donor","log",log,sizeof log)) {
        snprintf(notice,cap,"Cannot start the private household service."); return 1;
    }
    /* A free lock means no donor is starting in another window. */
    if (home_service_lock_available("donor")==1) {
        pid_t child=fork();
        if (child<0) { snprintf(notice,cap,"Cannot start sharing: %s.",strerror(errno)); return 1; }
        if (!child) {
            if (setsid()<0) _exit(125);
            pid_t donor=fork();
            if (donor<0) _exit(125);
            if (donor>0) _exit(0);
            signal(SIGHUP,SIG_IGN);
            int limit=getdtablesize();
            for (int fd=3; fd<limit; fd++) close(fd);
            int nullfd=open("/dev/null",O_RDONLY), logfd=open(log,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
            if (nullfd<0 || logfd<0 || dup2(nullfd,0)<0 || dup2(logfd,1)<0 || dup2(logfd,2)<0) _exit(125);
            if (nullfd>2) close(nullfd);
            if (logfd>2) close(logfd);
            /* An ordinary desktop allows very little locked memory, and the
             * strict household default would refuse the first model there.
             * Only when nothing was requested, this donor chooses per load:
             * locked if the allowance covers it, otherwise resident and not
             * locked, and reported as such. An explicit policy is inherited
             * untouched and still refuses loudly. */
            if (!getenv("LUMABRI_RESIDENCY_POLICY") && setenv("LUMABRI_RESIDENCY_POLICY","auto",1)) _exit(125);
            char *args[]={binary,"donor","--headless","--join",(char *)s->tracker,"--ram-gb",(char *)s->ram,NULL};
            execv(binary,args); _exit(127);
        }
        int status=0;
        while (waitpid(child,&status,0)<0) if (errno!=EINTR) break;
    }
    for (unsigned i=0; i<300 && !g_stopping; i++) {
        if (start_donor_live(&seen) && seen.compute_enabled) return 0;
        (void)poll(NULL,0,100);
    }
    snprintf(notice,cap,"Sharing did not start. See %.900s",log);
    return 1;
}

/* The fragment is the only place the token appears, and the opener is run
 * directly: no shell ever parses the address. The household key is removed
 * from the browser's environment. */
/* One line about what this computer's sharing service does with model memory,
 * from the service itself: an adopted donor keeps the policy it started with. */
static void start_memory_line(void) {
    HomeServiceSnapshot seen;
    if (!start_donor_live(&seen) || seen.residency==LMB_RESIDENCY_UNREPORTED) return;
    int automatic=seen.residency==LMB_RESIDENCY_AUTO_LOCKED || seen.residency==LMB_RESIDENCY_AUTO_UNLOCKED;
    if (!automatic) {
        printf("  Memory:         %s, requested explicitly%s\n",lmb_residency_name(seen.residency),
            seen.residency==LMB_RESIDENCY_ENGINE_LOCKED ? "; a model above the OS locked-memory allowance is refused" : "");
    } else if (!seen.memlock_limit || seen.memlock_limit==UINT64_MAX) {
        printf("  Memory:         model memory is locked in RAM\n");
    } else {
        printf("  Memory:         not locked for models above %llu MiB, this account's locked-memory allowance.\n"
               "                  Such models are still loaded fully into RAM. " LMB_MEMLOCK_UNLOCKED_NOTE "\n"
               "                  To lock it: %s\n",(unsigned long long)(seen.memlock_limit>>20),lmb_memlock_howto());
    }
}

static int start_open_browser(const char *url) {
#ifdef __APPLE__
    const char *opener="open";
#else
    const char *opener="xdg-open";
#endif
    pid_t child=fork();
    if (child<0) return -1;
    if (!child) {
        int nullfd=open("/dev/null",O_RDWR);
        if (nullfd<0 || dup2(nullfd,0)<0 || dup2(nullfd,1)<0 || dup2(nullfd,2)<0) _exit(126);
        if (nullfd>2) close(nullfd);
        (void)setsid();
        unsetenv("LUMABRI_TOKEN");
        execlp(opener,opener,url,(char *)NULL); _exit(127);
    }
    for (unsigned i=0; i<60; i++) {
        int status=0; pid_t done=waitpid(child,&status,WNOHANG);
        if (done==child) return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
        if (done<0 && errno!=EINTR) return -1;
        (void)poll(NULL,0,50);
    }
    return 0; /* Some openers stay in the foreground as the browser itself. */
}

static int cmd_start(int argc, char **argv) {
    int tui=0, browser=1, port_set=0; unsigned port=47380;
    for (int i=0; i<argc; i++) {
        if (!strcmp(argv[i],"--tui")) tui=1;
        else if (!strcmp(argv[i],"--no-browser")) browser=0;
        else if (!strcmp(argv[i],"--port") && i+1<argc) {
            char *end; unsigned long value=strtoul(argv[++i],&end,10);
            if (!*argv[i] || *end || !value || value>65535) goto usage;
            port=(unsigned)value; port_set=1;
        } else goto usage;
    }
    if (home_service_foreground())
        return home_fail("lumabri start uses the background service. Remove LUMABRI_HOME_FOREGROUND and retry.");
    if (tui && (!isatty(0) || !isatty(1)))
        return home_fail("lumabri start --tui needs a terminal.");
    if (home_private_network()) return 1;
    HomeSettings s; home_settings_load(&s);
    if (home_settings_activate(&s)) return home_fail("Cannot activate the saved household credentials.");
    char notice[1200]="";
    const char *household="joined earlier";
    if (s.owner || !s.tracker[0]) {
        /* Exactly the TUI /create path: it makes the key and saves the
         * settings on first use, and adopts our own live tracker afterwards. */
        int fresh=!s.tracker[0]; pid_t unused=0;
        if (home_tracker_resume(&s,&unused,notice,sizeof notice)) {
            fprintf(stderr,"%s\n",notice[0] ? notice : "The household could not be started."); return 1;
        }
        household=fresh ? "created now" : "yours";
    } else if (!s.token[0] || home_connection_check(s.tracker,NULL)) {
        return home_fail("This computer joined the household at %.200s, which is not reachable now. "
            "Start Lumabri on that computer first, or use lumabri start --tui to join another.",s.tracker);
    }
    /* A first start should not point the page at a folder that is absent. */
    char default_models[512];
    if (!checked_printf(default_models,sizeof default_models,"%s/.lumabri/models",getenv("HOME") ? getenv("HOME") : ".") &&
        !strcmp(default_models,s.models)) mkdir_p(s.models);
    notice[0]=0;
    int degraded=start_sharing(&s,notice,sizeof notice);
    if (degraded) fprintf(stderr,"%s\n",notice);

    char path[1200];
    if (checked_printf(path,sizeof path,"%s/.lumabri/api-access",getenv("HOME") ? getenv("HOME") : ".")) return 1;
    int dir=lmb_api_access_dir(path,1);
    if (dir<0) return home_fail("Cannot open the private API access directory.");
    HomeServiceSnapshot api;
    if (!home_service_query("api",HOME_SVC_STATUS,NULL,&api)) {
        /* Reuse the page that is already being served instead of asking for
         * a second gateway on another port. */
        unsigned live=0; char tail=0;
        if (sscanf(api.name,"http://127.0.0.1:%u%c",&live,&tail)!=1 || !live || live>65535) {
            close(dir);
            return home_fail("The web gateway is running behind a private TLS proxy. Open its HTTPS address, or run lumabri api stop first.");
        }
        if (!port_set) port=live;
    }
    int started=1;
    api_start_quiet=1;
    for (unsigned attempt=0; attempt<40 && started && !g_stopping; attempt++) {
        started=api_start(dir,port,s.tracker,NULL,path);
        /* Another start can be between taking the keeper lock and ready. */
        if (started && (home_service_query("api",HOME_SVC_STATUS,NULL,&api) || api.phase==2)) break;
        if (started) (void)poll(NULL,0,100);
    }
    api_start_quiet=0;
    if (started) { close(dir); return home_fail("The web page could not be started on 127.0.0.1:%u.",port); }

    char token[98]={0}; int created=0, taken=start_local_user(dir,token,&created);
    if (taken<0) { close(dir); return home_fail("Cannot prepare the local web sign-in. The API access database is busy or unsafe."); }
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
    if (!degraded) start_memory_line();
    if (granted) printf("  %u approved model%s added to the local sign-in.\n",granted,granted==1 ? " was" : "s were");
    if (missing) printf("  %u approved model%s could not be added to the local sign-in.\n",missing,missing==1 ? "" : "s");
    if (taken)
        printf("\nA web user named \"" LMB_START_USER "\" already exists and its token is not saved on this computer.\n"
               "Sign in on the page with that token, or run: lumabri api revoke " LMB_START_USER "\nand start again.\n");
    fflush(stdout);
    if (tui) { lmb_api_wipe(link,sizeof link); return cmd_home(); }
    int opened=browser && !start_open_browser(taken ? address : link);
    if (browser && !opened) fprintf(stderr,"\nNo browser could be opened from here.\n");
    if (!opened && !taken)
        printf("\nOpen this private link in a browser on this computer. It signs you in; do not share it.\n  %s\n",link);
    lmb_api_wipe(link,sizeof link);
    return degraded || missing ? 1 : 0;
usage:
    fprintf(stderr,"lumabri start [--tui] [--no-browser] [--port N]\n"
        "  Creates this computer's household if there is none, starts sharing and the\n"
        "  local web page, then opens that page in the default browser.\n"
        "  --tui         open the terminal workspace instead of the browser\n"
        "  --no-browser  print the private sign-in link instead of opening it\n"
        "  --port N      local web page port (default 47380, or the running page)\n");
    return 2;
}
#endif
