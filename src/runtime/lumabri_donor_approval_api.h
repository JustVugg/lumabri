/* The "Share resources" decision, offered to the page served on this computer.
 * It is the same keeper RPC the TUI sends, with the same three fences: keeper
 * incarnation, immutable allocation ID and offer revision. Nothing is decided
 * here, nothing is accepted by default, and no grant is written by this file.
 *
 * Authority. The donor keeper answers only a same-user peer on its private
 * socket (owner-only directory, kernel peer credentials). This gateway child
 * runs as that OS user, so it speaks for the owner of this computer and of no
 * other: there is no address or computer name in a request. The caller needs
 * the explicit management capability, and the route is refused when the
 * gateway is published through a TLS proxy, because a management credential
 * used from another computer must not authorize this donor for its owner. */
#ifndef LMB_DONOR_APPROVAL_API_H
#define LMB_DONOR_APPROVAL_API_H

static const char *donor_approval_phase(uint32_t phase) {
    static const char *const names[]={"idle","pending","accepted","loading","segment_ready",
        "starting_host","ready","declined","failed","released"};
    return phase<sizeof names/sizeof *names ? names[phase] : "invalid";
}
/* 0: this household's live donor. 1: no live donor. 2: another household's. */
static int donor_approval_observe(const char *tracker,HomeServiceSnapshot *s) {
    if (home_service_query("donor",HOME_SVC_STATUS,NULL,s) || s->state!=HOME_SVC_RUNNING) return 1;
    return strcmp(s->tracker,tracker) ? 2 : 0;
}
/* Whether this load is, or would be, page-locked on this computer. Under the
 * automatic choice it is the donor's own rule: the whole reservation against
 * the allowance. "null" when this service did not report its policy. */
static const char *donor_approval_locked(const HomeServiceSnapshot *s) {
    switch (s->residency) {
    case LMB_RESIDENCY_ENGINE_LOCKED: case LMB_RESIDENCY_PROCESS_LOCKED: return "true";
    case LMB_RESIDENCY_OBSERVE: return "false";
    case LMB_RESIDENCY_AUTO_LOCKED: case LMB_RESIDENCY_AUTO_UNLOCKED:
        return !s->memlock_limit || s->memlock_limit==UINT64_MAX || s->offer.ram_bytes<=s->memlock_limit ? "true" : "false";
    default: return "null";
    }
}
static int donor_approval_memory_json(Cap *body,const HomeServiceSnapshot *s) {
    int unlocked=lmb_residency_unlocked(s->residency);
    if (api_addf(body,",\"memory\":{\"mode\":\"%s\",\"locked\":%s,\"memlock_limit_bytes\":",lmb_residency_name(s->residency),
            unlocked<0 ? "null" : unlocked ? "false" : "true")) return -1;
    if (!s->memlock_limit || s->memlock_limit==UINT64_MAX ? cap_str(body,"null") :
            api_addf(body,"%llu",(unsigned long long)s->memlock_limit)) return -1;
    /* The warning is for weights held without locks, and for the automatic
     * choice whenever a larger model would be held that way. */
    int warn=unlocked==1 || (s->residency==LMB_RESIDENCY_AUTO_LOCKED && s->memlock_limit && s->memlock_limit!=UINT64_MAX);
    char text[900]="";
    if (unlocked==1) snprintf(text,sizeof text,LMB_MEMLOCK_UNLOCKED_NOTE " To lock it: %s",lmb_memlock_howto());
    else if (warn) snprintf(text,sizeof text,"Models above %llu MiB are held in RAM without locking on this computer. "
        LMB_MEMLOCK_UNLOCKED_NOTE " To lock it: %s",(unsigned long long)(s->memlock_limit>>20),lmb_memlock_howto());
    return api_addf(body,",\"warning\":%s,\"detail\":",warn ? "true" : "false") || api_json_text(body,text) || cap_str(body,"}");
}
static int donor_approval_request_json(Cap *body,const HomeServiceSnapshot *s) {
    const LmbHomeOffer *o=&s->offer; char allocation[65],requester[65];
    lmb_hex(allocation,o->id,32); lmb_hex(requester,o->requester,32);
    /* Same eligibility as the TUI's third choice; the keeper decides again. */
    int repeat=s->phase==LMB_HOME_PENDING && home_resident_required() && !o->hybrid_role;
    /* Household requesters name a load home-REQUESTER-ALLOCATION-NAME. Show
     * the folder name only when both prefixes are the ones in this offer. */
    const char *name=o->model;
    if (!strncmp(name,"home-",5) && !strncmp(name+5,requester,12) && name[17]=='-' &&
        !strncmp(name+18,allocation,12) && name[30]=='-' && name[31]) name+=31;
    return api_addf(body,"{\"allocation\":\"%s\",\"requester\":\"%s\",\"name\":",allocation,requester) ||
        api_json_text(body,name) || cap_str(body,",\"model\":") ||
        api_json_text(body,o->model) || cap_str(body,",\"adapter\":") || api_json_text(body,o->model_type) ||
        api_addf(body,",\"begin\":%u,\"end\":%u,\"layers\":%u,\"ram_bytes\":%llu,\"metadata_bytes\":%llu,",
            o->begin,o->end,o->layers,(unsigned long long)o->ram_bytes,(unsigned long long)o->disk_bytes) ||
        api_addf(body,"\"context\":%u,\"sessions\":%u,\"threads\":%u,\"max_new\":%u,",o->context,
            o->sessions ? o->sessions : 1,o->threads<s->threads ? o->threads : s->threads,o->max_new) ||
        api_addf(body,"\"runs_edge\":%s,\"expert_accelerator\":%s,\"repeat_supported\":%s,\"memory_locked\":%s}",
            o->runs_edge ? "true" : "false",o->hybrid_role==LMB_HYBRID_ACCELERATOR ? "true" : "false",
            repeat ? "true" : "false",donor_approval_locked(s));
}
static int donor_approval_json(Cap *body,const HomeServiceSnapshot *s,int observed) {
    if (observed) return api_addf(body,"{\"schema\":1,\"scope\":\"this_computer\",\"sharing\":false,"
        "\"reason\":\"%s\",\"pending\":false,\"request\":null,\"automatic_consent\":false}",
        observed==2 ? "sharing_with_another_household" : "not_sharing");
    char instance[65]; lmb_hex(instance,s->instance,32);
    int shown=s->has_offer && s->phase!=LMB_HOME_IDLE;
    return api_addf(body,"{\"schema\":1,\"scope\":\"this_computer\",\"sharing\":true,\"instance\":\"%s\","
            "\"revision\":\"%llu\",\"computer\":",instance,(unsigned long long)s->revision) ||
        api_json_text(body,s->name) ||
        api_addf(body,",\"phase\":\"%s\",\"state\":",donor_approval_phase(s->phase)) ||
        api_json_text(body,lmb_home_phase_name((LmbHomePhase)s->phase)) ||
        cap_str(body,",\"detail\":") || api_json_text(body,s->detail) ||
        api_addf(body,",\"pending\":%s,\"decision_seconds\":%u,\"request\":",
            shown && s->phase==LMB_HOME_PENDING ? "true" : "false",LMB_HOME_DECISION_MS/1000u) ||
        (shown ? donor_approval_request_json(body,s) : cap_str(body,"null")) ||
        donor_approval_memory_json(body,s) ||
        cap_str(body,",\"automatic_consent\":false}");
}
/* The page repeats the exact request it displayed. A decimal string keeps the
 * 64-bit revision out of JavaScript's number rounding. */
static int donor_approval_parse(const char *text,size_t size,uint32_t *operation,HomeServiceSnapshot *expected) {
    LmbJsonToken tokens[16]; LmbJson j; int f[4];
    const char *const names[]={"action","instance","allocation","revision"};
    char verb[16],instance[65],allocation[65],canonical[65],revision[21];
    memset(expected,0,sizeof *expected);
    if (!size || size>512 || lmb_json_parse(&j,text,size,tokens,16) || lmb_json_fields(&j,0,names,4,f) ||
        f[0]<0 || f[1]<0 || f[2]<0 || f[3]<0 || lmb_api_json_string(&j,(unsigned)f[0],verb,sizeof verb) ||
        lmb_api_json_string(&j,(unsigned)f[1],instance,sizeof instance) ||
        lmb_api_json_string(&j,(unsigned)f[2],allocation,sizeof allocation) ||
        lmb_api_json_string(&j,(unsigned)f[3],revision,sizeof revision) ||
        strlen(instance)!=64 || lmb_unhex(expected->instance,instance,32) || !lmb_home_nonzero(expected->instance,32) ||
        strlen(allocation)!=64 || lmb_unhex(expected->offer.id,allocation,32) || !lmb_home_nonzero(expected->offer.id,32) ||
        revision[0]<'1' || revision[0]>'9') return -1;
    lmb_hex(canonical,expected->instance,32); if (strcmp(canonical,instance)) return -1;
    lmb_hex(canonical,expected->offer.id,32); if (strcmp(canonical,allocation)) return -1;
    for (const char *p=revision;*p;p++) {
        if (*p<'0' || *p>'9' || expected->revision>(UINT64_MAX-(unsigned)(*p-'0'))/10) return -1;
        expected->revision=expected->revision*10+(unsigned)(*p-'0');
    }
    if (!strcmp(verb,"accept")) *operation=HOME_SVC_ACCEPT;
    else if (!strcmp(verb,"decline")) *operation=HOME_SVC_DECLINE;
    else if (!strcmp(verb,"allow_repeat")) *operation=HOME_SVC_ALLOW_REPEAT;
    else if (!strcmp(verb,"revoke_repeat")) *operation=HOME_SVC_REVOKE_REPEAT;
    else return -1;
    return 0;
}
static void api_donor_approval(int fd,const char *tracker,int proxied,const char *input,size_t size) {
    if (proxied) { api_error(fd,403,"donor_approval_local_page_only"); return; }
    HomeServiceSnapshot current; Cap body={0};
    int observed=donor_approval_observe(tracker,&current);
    if (!input) {
        if (donor_approval_json(&body,&current,observed)) api_error(fd,500,"allocation_failed");
        else (void)api_response(fd,200,body.p);
        free(body.p); return;
    }
    uint32_t operation=0; HomeServiceSnapshot expected,after;
    if (donor_approval_parse(input,size,&operation,&expected)) { api_error(fd,400,"invalid_donor_approval_request"); return; }
    if (observed) { api_error(fd,503,"local_donor_unavailable"); return; }
    /* What the person saw must still be what the keeper holds. The keeper
     * repeats this comparison itself; a lost reply is never retried here. */
    if (memcmp(current.instance,expected.instance,32) || !current.has_offer ||
        memcmp(current.offer.id,expected.offer.id,32) || current.revision!=expected.revision ||
        (operation!=HOME_SVC_REVOKE_REPEAT && current.phase!=LMB_HOME_PENDING) ||
        (operation==HOME_SVC_ALLOW_REPEAT && (!home_resident_required() || current.offer.hybrid_role))) {
        api_error(fd,409,"donor_request_changed_review_again"); return;
    }
    if (home_service_query("donor",operation,&current,&after)) {
        api_error(fd,409,"donor_decision_not_confirmed_review_again"); return;
    }
    int same=after.has_offer && !memcmp(after.offer.id,expected.offer.id,32);
    const char *result=operation==HOME_SVC_REVOKE_REPEAT ? "observed" :
        same && after.phase==LMB_HOME_PENDING ? "pending" :
        same && after.phase>=LMB_HOME_ACCEPTED && after.phase<=LMB_HOME_READY ? "accepted" :
        operation==HOME_SVC_DECLINE ? "declined" : "not_accepted";
    fprintf(stderr,"[donor-approval] action=%s result=%s\n",operation==HOME_SVC_ACCEPT ? "accept" :
        operation==HOME_SVC_DECLINE ? "decline" : operation==HOME_SVC_ALLOW_REPEAT ? "allow_repeat" : "revoke_repeat",result);
    char allocation[65]; lmb_hex(allocation,expected.offer.id,32);
    if (api_addf(&body,"{\"schema\":1,\"allocation\":\"%s\",\"result\":\"%s\",\"phase\":\"%s\",\"detail\":",
            allocation,result,donor_approval_phase(after.phase)) || api_json_text(&body,after.detail) ||
        cap_str(&body,",\"automatic_consent\":false}")) api_error(fd,500,"allocation_failed");
    else (void)api_response(fd,200,body.p);
    free(body.p);
}
#endif
