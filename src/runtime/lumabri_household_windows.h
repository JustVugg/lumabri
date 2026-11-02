/* Start the shared tracker through its native keeper. Persist the household
 * only after READY and an authenticated round trip to this machine's key. */
#ifndef LMB_HOUSEHOLD_WINDOWS_H
#define LMB_HOUSEHOLD_WINDOWS_H
static int home_settings_equal_windows(const HomeSettings *a,const HomeSettings *b) {
    return a->owner==b->owner && !strcmp(a->tracker,b->tracker) && !strcmp(a->token,b->token) &&
        !strcmp(a->models,b->models) && !strcmp(a->ram,b->ram) &&
        !strcmp(a->sessions,b->sessions) && !strcmp(a->context,b->context);
}
static int home_create_windows(HomeSettings *current) {
    char path[1200];
    if (lmb_private_default_path(path,sizeof path,"household-create.lock")) return -1;
    HANDLE guard=lmb_private_open(path,1,1,NULL);
    if (guard==INVALID_HANDLE_VALUE) return -1;
    if (lmb_private_lock(guard,1)) {CloseHandle(guard);return -4;}
    int rc=-1;HomeSettings next=*current,saved;
    HomeServiceSnapshot active,created={0};
    /* A different workspace may have committed since this screen opened. */
    if (home_settings_load_windows(&saved) || !home_settings_equal_windows(&saved,current)) {rc=-4;goto done;}
    if (!home_service_query("tracker",HOME_SVC_STATUS,NULL,&active)) {
        rc=current->owner && !strcmp(active.tracker,current->tracker) &&
            !home_connection_check_identity_windows(current->tracker,g_sec_pk) ? 0 : -4;
        goto done;
    }
    const char *roles[]={"donor","prepare"};
    for (unsigned i=0;i<2;i++) if (!home_service_query(roles[i],HOME_SVC_STATUS,NULL,&active)) {rc=-4;goto done;}
    int port=lmb_home_port_base();char host[64],port_text[16];
    if (port<0 || lmb_home_advertise_ip(host,sizeof host)) goto done;
    if (!next.owner || !next.token[0]) {
        uint8_t random[32];lmb_random(random,sizeof random);lmb_hex(next.token,random,sizeof random);
        SecureZeroMemory(random,sizeof random);
    }
    next.owner=1;snprintf(next.tracker,sizeof next.tracker,"%s:%d",host,port);
    snprintf(port_text,sizeof port_text,"%d",port);
    if (home_settings_activate_windows(&next) || home_service_ensure()) goto done;
    const char *args[]={"--internal-tracker","--port",port_text,"--address",next.tracker};
    if (home_service_keeper_start_owned_windows("tracker",args,5,&created)) {rc=-5;goto done;}
    rc=home_connection_check_identity_windows(next.tracker,g_sec_pk);
    if (!rc && home_settings_save_windows(&next)) rc=-3;
    if (!rc) *current=next;
done:
    if (rc) {
        /* Never stop a concurrent winner or a keeper discovered by PID. */
        if (created.pid) {
            HomeServiceSnapshot stopped;
            if (home_service_query("tracker",HOME_SVC_STOP,&created,&stopped)) rc=-6;
        }
        (void)home_settings_activate_windows(current);
    }
    SecureZeroMemory(&next,sizeof next);SecureZeroMemory(&saved,sizeof saved);
    lmb_private_unlock(guard);CloseHandle(guard);return rc;
}
#endif
