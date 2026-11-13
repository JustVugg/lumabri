/* The page's side of pairing with a short code. Included by lumabri_api.h.
 *
 *   GET  /api/v1/household                     which household this computer is in
 *   POST /api/v1/household/pairing             {"action":"create"} or {"action":"cancel","id":ID}
 *   GET  /api/v1/household/pairing/ID          waiting, used, expired, locked, cancelled, unknown
 *   POST /api/v1/household/join                {"code":CODE,"address":"host:port","ram_gb":N}
 *
 * All four need a sign-in with the management permission and the loopback
 * gateway: a code admits a computer to the household, and joining moves this
 * computer to another one. The protocol itself is in lumabri_pairing_client.h;
 * a code is never written to a log here, and error replies name a reason, not
 * a value.
 *
 * Joining from the page ends the current household's services on this
 * computer. The exchange runs first, while everything still works, so a
 * wrong code changes nothing. Only after the other household has proven that
 * it knows the code and accepted its own key are the settings saved and the
 * local services restarted, with the same two commands a person would type:
 * `lumabri service stop`, then `lumabri start`. */
#ifndef LUMABRI_PAIRING_HTTP_H
#define LUMABRI_PAIRING_HTTP_H

/* Text that needs no escaping inside a JSON string: a host name or an address. */
static int api_pairing_plain(const char *text) {
    for (const char *c=text; *c; c++)
        if (!isalnum((unsigned char)*c) && !strchr(".:[]_-",*c)) return 0;
    return 1;
}

static int api_pairing_error(int rc, unsigned *status, const char **code) {
    *status=409;
    switch (rc) {
    case LMB_PAIR_OK: return 0;
    case LMB_PAIR_E_CONNECT: *status=503; *code="household_unreachable"; break;
    case LMB_PAIR_E_UNSUPPORTED: *code="pairing_not_supported_by_household"; break;
    case LMB_PAIR_E_NOT_OWNER: *code="pairing_owner_only"; break;
    case LMB_PAIR_E_CODE: *status=400; *code="invalid_pairing_code"; break;
    case LMB_PAIR_E_FAILED: *status=403; *code="pairing_failed"; break;
    case LMB_PAIR_E_IDENTITY: *code="household_identity_mismatch"; break;
    case LMB_PAIR_E_PINNED: *code="household_address_pinned_to_another_identity"; break;
    default: *status=502; *code="pairing_refused"; break;
    }
    return 1;
}

/* Detached from the gateway, which the first command stops. */
static int api_pairing_restart(unsigned port) {
    char dir[1024], binary[1200], log[1200], port_text[16];
    exe_dir(dir,sizeof dir);
    if (checked_printf(binary,sizeof binary,"%s/lumabri",dir) ||
        checked_printf(log,sizeof log,"%s/.lumabri/household-switch.log",getenv("HOME") ? getenv("HOME") : ".")) return -1;
    snprintf(port_text,sizeof port_text,"%u",port);
    fflush(NULL);
    pid_t child=fork();
    if (child<0) return -1;
    if (!child) {
        if (setsid()<0) _exit(125);
        pid_t helper=fork();
        if (helper) _exit(helper<0 ? 125 : 0);
        signal(SIGHUP,SIG_IGN); signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL);
        int limit=getdtablesize();
        for (int fd=0; fd<limit; fd++) close(fd);
        int nullfd=open("/dev/null",O_RDONLY), logfd=open(log,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
        if (nullfd!=0 || logfd<0 || dup2(logfd,1)<0 || dup2(logfd,2)<0) _exit(125);
        if (logfd>2) close(logfd);
        static const char *const inherited[]={"LUMABRI_READY_FD","LUMABRI_HOME_LISTEN_FD","LUMABRI_NODE_CONTROL_FD",
            "LUMABRI_ALLOCATION_LIFETIME_FD","LUMABRI_COMPUTE_SOCKET"};
        for (unsigned i=0; i<sizeof inherited/sizeof *inherited; i++) unsetenv(inherited[i]);
        (void)poll(NULL,0,700); /* the reply to the page leaves first */
        pid_t stop=fork();
        if (!stop) { execl(binary,binary,"service","stop",(char *)NULL); _exit(127); }
        int status=0;
        while (stop>0 && waitpid(stop,&status,0)<0) if (errno!=EINTR) { status=-1; break; }
        if (stop<0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
            dprintf(2,"[household-switch] local services did not stop; run: lumabri service stop, then lumabri start\n");
            _exit(1);
        }
        execl(binary,binary,"start","--no-browser","--port",port_text,(char *)NULL);
        _exit(127);
    }
    int status=0;
    while (waitpid(child,&status,0)<0) if (errno!=EINTR) return -1;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

static void api_pairing_join(int fd, unsigned port, const HomeSettings *current, const char *code,
                             const char *address, uint32_t ram_gb) {
    HomeSettings next=*current; uint8_t expected[32]; int discovered=0;
    unsigned status=409; const char *reason=NULL;
    char checked[LMB_PAIR_CODE_LEN+1];
    if (lmb_pair_code_normalize(checked,code)) { api_error(fd,400,"invalid_pairing_code"); return; }
    lmb_cpace_wipe(checked,sizeof checked);
    if (address[0]) {
        int base=lmb_home_port_base();
        if (!lmb_sec_addr_ok(address) || !api_pairing_plain(address) || (strchr(address,':') ? checked_printf(next.tracker,sizeof next.tracker,"%s",address) :
            (base<0 || checked_printf(next.tracker,sizeof next.tracker,"%s:%d",address,base)))) {
            api_error(fd,400,"invalid_household_address"); return;
        }
    } else {
        /* This computer's own household answers discovery too; it is not a candidate. */
        uint8_t nonce[16]; lmb_random(nonce,sizeof nonce);
        LmbHomeFound found[LMB_HOME_DISC_MAX]; int count=lmb_home_find(found,nonce), others=0, chosen=-1;
        for (int i=0; i<count; i++) if (memcmp(found[i].identity,g_sec_pk,32)) { others++; chosen=i; }
        if (others!=1) { api_error(fd,others ? 409 : 404,others ? "several_households_found" : "no_household_found"); return; }
        snprintf(next.tracker,sizeof next.tracker,"%s",found[chosen].address);
        memcpy(expected,found[chosen].identity,32); discovered=1;
    }
    if (!strcmp(next.tracker,current->tracker)) { api_error(fd,409,"already_in_this_household"); return; }
    if (ram_gb) snprintf(next.ram,sizeof next.ram,"%u",(unsigned)ram_gb);
    LmbPairJoined joined;
    if (api_pairing_error(lmb_pair_join(next.tracker,code,discovered ? expected : NULL,&joined),&status,&reason)) {
        api_error(fd,status,reason); return;
    }
    memcpy(next.token,joined.household_key,sizeof next.token); next.owner=0;
    if (setenv("LUMABRI_TOKEN",next.token,1) || home_connection_check(next.tracker,joined.tracker_identity)) reason="household_key_rejected";
    else if (home_settings_save(&next)) reason="household_settings_not_saved";
    lmb_cpace_wipe(&joined,sizeof joined);
    if (reason) { (void)setenv("LUMABRI_TOKEN",current->token,1); lmb_cpace_wipe(&next,sizeof next); api_error(fd,500,reason); return; }
    char body[420];
    snprintf(body,sizeof body,"{\"schema\":1,\"joined\":true,\"tracker\":\"%.255s\",\"restarting\":%s}\n",
             next.tracker,api_pairing_restart(port) ? "false" : "true");
    lmb_cpace_wipe(&next,sizeof next);
    (void)api_response(fd,202,body);
}

/* 1 when the request was one of the household routes and has been answered. */
static int api_pairing_route(int fd, int access_dir, unsigned port, const LmbApiUser *user,
                             const LmbApiHttp *request, int remote) {
    static const char household[]="/api/v1/household", pairing[]="/api/v1/household/pairing", join[]="/api/v1/household/join";
    size_t prefix=sizeof pairing-1;
    int is_household=!strcmp(request->path,household), is_join=!strcmp(request->path,join);
    int is_pairing=!strncmp(request->path,pairing,prefix) && (!request->path[prefix] || request->path[prefix]=='/');
    if (!is_household && !is_join && !is_pairing) return 0;
    int get=!strcmp(request->method,"GET") && !request->length;
    int post=!strcmp(request->method,"POST") && request->has_length && request->length && request->json;
    if (remote) { api_error(fd,403,"household_local_gateway_only"); return 1; }
    if (!lmb_management_allows(access_dir,user)) { api_error(fd,403,"management_permission_required"); return 1; }
    const char *id_text=is_pairing && request->path[prefix] ? request->path+prefix+1 : "";
    if (!(get && (is_household || (is_pairing && *id_text))) && !(post && (is_join || (is_pairing && !*id_text)))) {
        api_error(fd,400,"unsupported_endpoint_or_framing"); return 1;
    }
    char body[400]="", action[16]="", id_field[2*LMB_PAIR_ID_LEN+1]="", code[40]="", address[256]="";
    uint32_t ram_gb=0;
    if (post) {
        size_t at=0; uint64_t deadline=lmb_io_monotonic_ms()+5000;
        if (request->length>=sizeof body) { api_error(fd,413,"management_request_too_large"); return 1; }
        while (at<request->length && lmb_io_monotonic_ms()<deadline) {
            ssize_t got=recv(fd,body+at,request->length-at,0);
            if (got<0 && errno==EINTR) continue;
            if (got<=0) break;
            at+=(size_t)got;
        }
        if (at!=request->length) { api_error(fd,408,"request_body_timeout"); return 1; }
        LmbJsonToken tokens[10]; LmbJson j; int f[3];
        const char *const pairing_keys[]={"action","id"}, *const join_keys[]={"code","address","ram_gb"};
        int bad=lmb_json_parse(&j,body,at,tokens,10) || lmb_json_fields(&j,0,is_join ? join_keys : pairing_keys,is_join ? 3 : 2,f);
        if (!bad && is_join) bad=f[0]<0 || lmb_api_json_string(&j,(unsigned)f[0],code,sizeof code) ||
            (f[1]>=0 && lmb_api_json_string(&j,(unsigned)f[1],address,sizeof address)) ||
            (f[2]>=0 && (lmb_json_uint(&j,(unsigned)f[2],&ram_gb) || !ram_gb || ram_gb>1048576));
        if (!bad && !is_join) bad=f[0]<0 || lmb_api_json_string(&j,(unsigned)f[0],action,sizeof action) ||
            (f[1]>=0 && lmb_api_json_string(&j,(unsigned)f[1],id_field,sizeof id_field)) ||
            (strcmp(action,"create") ? strcmp(action,"cancel") || f[1]<0 : f[1]>=0);
        lmb_cpace_wipe(body,sizeof body);
        if (bad) { lmb_cpace_wipe(code,sizeof code); api_error(fd,400,"invalid_household_request"); return 1; }
        id_text=id_field;
    }
    HomeSettings s; home_settings_load(&s);
    unsigned status=409; const char *reason=NULL; char reply[512];
    if (is_household) {
        snprintf(reply,sizeof reply,"{\"schema\":1,\"tracker\":\"%.255s\",\"owner\":%s}\n",
                 api_pairing_plain(s.tracker) ? s.tracker : "",s.owner ? "true" : "false");
        (void)api_response(fd,200,reply);
    } else if (is_join) {
        api_pairing_join(fd,port,&s,code,address,ram_gb);
        lmb_cpace_wipe(code,sizeof code);
    } else if (!s.owner || !s.token[0] || setenv("LUMABRI_TOKEN",s.token,1)) api_error(fd,409,"pairing_owner_only");
    else if (post && !strcmp(action,"create")) {
        LmbPairOpened opened; char shown[LMB_PAIR_CODE_LEN+2], id_hex[2*LMB_PAIR_ID_LEN+1];
        if (api_pairing_error(lmb_pair_owner_open(s.tracker,home_pair_lifetime(),&opened),&status,&reason)) api_error(fd,status,reason);
        else {
            lmb_pair_code_format(shown,opened.code); lmb_hex(id_hex,opened.id,LMB_PAIR_ID_LEN);
            snprintf(reply,sizeof reply,"{\"schema\":1,\"id\":\"%s\",\"code\":\"%s\",\"expires_in\":%u,\"max_attempts\":%u,\"address\":\"%.255s\"}\n",
                     id_hex,shown,(unsigned)opened.lifetime_s,(unsigned)opened.max_attempts,
                     api_pairing_plain(s.tracker) ? s.tracker : "");
            (void)api_response(fd,200,reply);
            lmb_cpace_wipe(reply,sizeof reply); lmb_cpace_wipe(shown,sizeof shown);
        }
        lmb_cpace_wipe(&opened,sizeof opened);
    } else {
        uint8_t id[LMB_PAIR_ID_LEN]; LmbPairStatus state;
        if (strlen(id_text)!=2*LMB_PAIR_ID_LEN || lmb_unhex(id,id_text,LMB_PAIR_ID_LEN)) api_error(fd,400,"invalid_household_request");
        else if (post) {
            int rc=lmb_pair_owner_cancel(s.tracker,id);
            if (rc && rc!=LMB_PAIR_E_REFUSED && api_pairing_error(rc,&status,&reason)) api_error(fd,status,reason);
            else (void)api_response(fd,200,rc ? "{\"schema\":1,\"cancelled\":false}\n" : "{\"schema\":1,\"cancelled\":true}\n");
        } else if (api_pairing_error(lmb_pair_owner_status(s.tracker,id,&state),&status,&reason)) api_error(fd,status,reason);
        else {
            char joiner[65]; lmb_hex(joiner,state.joiner,32);
            for (char *c=state.source; *c; c++) if (!isalnum((unsigned char)*c) && !strchr(".:[]_-%",*c)) *c='?';
            snprintf(reply,sizeof reply,"{\"schema\":1,\"state\":\"%s\",\"expires_in\":%u,\"failed\":%u,\"max_attempts\":%u,"
                     "\"joiner\":\"%.16s\",\"source\":\"%s\"}\n",lmb_pair_state_name(state.state),(unsigned)state.remaining_s,
                     (unsigned)state.failed,(unsigned)state.max_attempts,state.state==LMB_PAIR_USED ? joiner : "",state.source);
            (void)api_response(fd,200,reply);
        }
    }
    lmb_cpace_wipe(&s,sizeof s);
    return 1;
}
#endif
