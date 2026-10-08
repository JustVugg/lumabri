/* Shared allocation management, over the same authenticated service as chat.
 * HTTP callers cannot supply network addresses, checkpoint roots or commands.
 * The local operator grants management explicitly; read-only operators and
 * inference users do not inherit it. No action creates a donor approval. */
#ifndef LMB_MANAGEMENT_H
#define LMB_MANAGEMENT_H

static int lmb_management_request(const char *body, size_t size, uint32_t *action, LmbHostControl *expected) {
    LmbJsonToken tokens[16]; LmbJson j; int f[3];
    const char *const names[]={"action","instance","revision"};
    char verb[16],instance[65],revision[21];
    memset(expected,0,sizeof *expected);
    if (!size || size>512 || lmb_json_parse(&j,body,size,tokens,16) || lmb_json_fields(&j,0,names,3,f) ||
        f[0]<0 || f[1]<0 || f[2]<0 || lmb_api_json_string(&j,(unsigned)f[0],verb,sizeof verb) ||
        lmb_api_json_string(&j,(unsigned)f[1],instance,sizeof instance) ||
        lmb_api_json_string(&j,(unsigned)f[2],revision,sizeof revision) || strlen(instance)!=64 ||
        lmb_unhex(expected->instance,instance,32) || !lmb_route_nonzero(expected->instance) ||
        revision[0]<'1' || revision[0]>'9') return -1;
    /* Decimal string avoids rounding uint64 process fences in JavaScript. */
    for (const char *p=revision;*p;p++) {
        if (*p<'0' || *p>'9' || expected->revision>(UINT64_MAX-(unsigned)(*p-'0'))/10) return -1;
        expected->revision=expected->revision*10+(unsigned)(*p-'0');
    }
    if (!strcmp(verb,"drain")) *action=LMB_HOST_CONTROL_DRAIN;
    else if (!strcmp(verb,"resume")) *action=LMB_HOST_CONTROL_RESUME;
    else if (!strcmp(verb,"retire")) *action=LMB_HOST_CONTROL_RETIRE;
    else return -1;
    return 0;
}
static int lmb_management_path(const char *path, uint8_t id[32]) {
    const char *prefix="/api/v1/allocations/"; size_t n=strlen(prefix);
    if (strncmp(path,prefix,n) || strlen(path)!=n+64+8 || strcmp(path+n+64,"/control")) return -1;
    char hex[65],canonical[65]; memcpy(hex,path+n,64); hex[64]=0;
    if (lmb_unhex(id,hex,32) || !lmb_route_nonzero(id)) return -1;
    lmb_hex(canonical,id,32); return strcmp(hex,canonical) ? -1 : 0;
}
static int api_management_record(Cap *body, const char *allocation, const LmbHostControl *state) {
    char instance[65]; lmb_hex(instance,state->instance,32);
    return api_addf(body,"{\"schema\":1,\"allocation\":\"%s\",\"instance\":\"%s\",\"revision\":\"%llu\","
        "\"state\":\"%s\",\"connections\":%u,\"admitted_requests\":%u,\"weights_unloaded\":false}",
        allocation,instance,(unsigned long long)state->revision,
        state->draining==LMB_HOST_CONTROL_RETIRED ? "retired" : !state->draining ? "accepting" :
            state->connections || state->requests ? "draining" : "drained",state->connections,state->requests);
}
static int api_management_retirement_record(Cap *body, const char *allocation, const LmbRetirement *r, int status) {
    char instance[65]; lmb_hex(instance,r->host.instance,32);
    return api_addf(body,"{\"schema\":1,\"allocation\":\"%s\",\"instance\":\"%s\",\"revision\":\"%llu\","
        "\"state\":\"%s\",\"phase\":%u,\"released_node_mask\":%u,\"complete\":%s}",
        allocation,instance,(unsigned long long)r->host.revision,
        status<0 ? "unknown_or_conflict" : r->phase==LMB_RET_DONE ? "released" : "retirement_pending",
        r->phase,r->released,r->phase==LMB_RET_DONE && status>=0 ? "true" : "false");
}
/* GET of a pending/completed retirement is a journal observation, not a
 * mutation or an assumption that unreachable nodes freed memory. It permits
 * explicit reconciliation even after the host was sealed or stopped. */
static int api_management_saved(int access, const char *tracker, const uint8_t id[32], LmbRetirement *out) {
    int dir=openat(access,"retirements",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir<0) return errno==ENOENT ? 1 : -1;
    if (lmb_api_private_fd(dir,1)) { close(dir); return -1; }
    char hex[65],name[80]; lmb_hex(hex,id,32); snprintf(name,sizeof name,"%s.retire",hex);
    int rc=lmb_retirement_load(dir,name,out); close(dir);
    if (!rc) {
        LmbResidentPlan *p=calloc(1,sizeof *p); uint8_t digest[64];
        if (!p || api_find_plan(tracker,id,p) || lmb_retirement_plan_hash(p,digest) ||
            memcmp(digest,out->plan_hash,64)) rc=-1;
        free(p);
    }
    return rc;
}
static void api_management(int fd, int access, const char *tracker, const LmbApiUser *user,
    const LmbApiHttp *request, const char *input, size_t size) {
    int writing=!strcmp(request->method,"POST"); uint8_t id[32];
    if (!(writing ? lmb_management_allows(access,user) : lmb_operator_allows(access,user))) {
        api_error(fd,403,writing ? "management_permission_required" : "operator_permission_required"); return;
    }
    if (lmb_management_path(request->path,id)) { api_error(fd,400,"invalid_allocation_control_path"); return; }
    uint32_t action=0; LmbHostControl expected={0},state={0};
    if (writing && lmb_management_request(input,size,&action,&expected)) {
        api_error(fd,400,"invalid_management_request"); return;
    }
    char allocation[65]; lmb_hex(allocation,id,32); Cap body={0}; unsigned http=200; int bad=0;
    if (action==LMB_HOST_CONTROL_RETIRE) {
        LmbRetirement r={0}; int conflict=0;
        int rc=api_retirement_execute(access,tracker,id,&expected,&r,&conflict);
        if (conflict) { api_error(fd,409,"allocation_changed_refresh_before_retry"); return; }
        http=rc<0 ? 503 : rc ? 202 : 200;
        bad=api_management_retirement_record(&body,allocation,&r,rc);
    } else {
        LmbRetirement r={0}; int saved=api_management_saved(access,tracker,id,&r);
        if (!saved) {
            if (writing) { api_error(fd,409,"retirement_started_cannot_resume"); return; }
            bad=api_management_retirement_record(&body,allocation,&r,0);
        } else if (saved<0) { api_error(fd,503,"retirement_state_unavailable"); return; }
        else {
            int rc=api_replica_execute(tracker,id,action,writing ? &expected : NULL,&state);
            if (rc) {
                api_error(fd,rc==LMB_HOST_CONTROL_CONFLICT ? 409 : 503,
                    rc==LMB_HOST_CONTROL_CONFLICT ? "allocation_changed_refresh_before_retry" : "allocation_control_unavailable");
                return;
            }
            bad=api_management_record(&body,allocation,&state);
        }
    }
    if (bad) api_error(fd,500,"allocation_failed"); else (void)api_response(fd,http,body.p);
    free(body.p);
}
#endif
