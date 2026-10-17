/* Authenticated management facade for the local-owner capacity probe.
 * Inspection never claims freshness. Check/select revalidate live evidence;
 * measure runs bounded inference only on already approved resident models.
 * There are no endpoints, paths, prompts, prices or imported measurements in
 * a request. Donor consent and routing are separate, explicit operations. */
#ifndef LMB_CAPACITY_HTTP_H
#define LMB_CAPACITY_HTTP_H

static int api_capacity_name_order(const void *a,const void *b) { return strcmp(a,b); }
static void api_capacity_list_http(int fd,int access) {
    int dir=lmb_capacity_dir(access,0);
    if (dir<0) {
        if (errno==ENOENT) (void)api_response(fd,200,"{\"schema\":1,\"scope\":\"historical_not_revalidated\",\"records\":[]}");
        else api_error(fd,503,"capacity_storage_unavailable");
        return;
    }
    char names[LMB_CAPACITY_RECORDS][33]; unsigned count=0,seen=0;
    int scan=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    DIR *entries=scan<0 ? NULL : fdopendir(scan);
    int bad=!entries;
    if (!entries && scan>=0) close(scan);
    if (entries) {
        struct dirent *item; errno=0;
        while ((item=readdir(entries))) {
            if (++seen>128) { bad=1; break; }
            if (!lmb_api_username(item->d_name)) continue;
            if (count==LMB_CAPACITY_RECORDS) { bad=1; break; }
            snprintf(names[count++],sizeof names[0],"%s",item->d_name);
        }
        if (errno) bad=1;
        closedir(entries);
    }
    qsort(names,count,sizeof names[0],api_capacity_name_order);
    Cap response={0};
    if (!bad) bad=cap_str(&response,"{\"schema\":1,\"scope\":\"historical_not_revalidated\",\"records\":[");
    for (unsigned i=0;!bad && i<count;i++) {
        LmbCapacity r; LmbCapacityCost unknown={0};
        bad=(i && cap_str(&response,",")) || cap_str(&response,"{\"name\":") || api_json_text(&response,names[i]);
        if (!bad && lmb_capacity_load(dir,names[i],&r)) bad=cap_str(&response,",\"state\":\"record_unavailable\",\"observation\":null}");
        else if (!bad) bad=cap_str(&response,",\"state\":\"not_revalidated\",\"observation\":") ||
            api_capacity_json(&response,&r,r.stable ? "historical" : "incomplete",&unknown) || cap_str(&response,"}");
    }
    close(dir);
    if (!bad) bad=cap_str(&response,"]}");
    if (bad) api_error(fd,503,"capacity_storage_unavailable");
    else (void)api_response(fd,200,response.p);
    free(response.p);
}
static void api_capacity_http(int fd,int access,const char *tracker,const char *path,const char *body,size_t size) {
    if (!body && !strcmp(path,"/api/v1/capacity")) { api_capacity_list_http(fd,access); return; }
    int measure=!strcmp(path,"/api/v1/capacity/measure"),check=!strcmp(path,"/api/v1/capacity/check"),
        select=!strcmp(path,"/api/v1/capacity/select"),remove_record=!strcmp(path,"/api/v1/capacity/remove");
    const char *const measure_keys[]={"record","clients","rounds","max_tokens","allocations"};
    const char *const check_keys[]={"record","ttft_ms","gap_ms","max_age_seconds"};
    const char *const select_keys[]={"ttft_ms","gap_ms","max_age_seconds","records"};
    const char *const remove_keys[]={"record"};
    const char *const *keys=measure ? measure_keys : check ? check_keys : select ? select_keys : remove_keys;
    unsigned fields_count=measure ? 5 : remove_record ? 1 : 4;
    LmbJsonToken tokens[64]; LmbJson j; int fields[5];
    char values[4+LMB_CAPACITY_MODELS][80],*args[4+LMB_CAPACITY_MODELS];
    unsigned count=0;
    int bad=(!measure && !check && !select && !remove_record) || !body || size>2048 ||
        lmb_json_parse(&j,body,size,tokens,64) || lmb_json_fields(&j,0,keys,fields_count,fields);
    for (unsigned i=0;!bad && i<fields_count;i++) if (fields[i]<0) bad=1;
    if (!bad && !select) {
        bad=lmb_api_json_string(&j,(unsigned)fields[0],values[count],33) || !lmb_api_username(values[count]);
        count++;
    }
    for (unsigned i=select ? 0 : 1;!bad && !remove_record && i<(select ? 3u : 4u);i++) {
        uint32_t n,low=1,high;
        if (measure) { high=i==1 ? LMB_CAPACITY_CLIENTS : i==2 ? LMB_CAPACITY_ROUNDS : 512; if (i==3) low=2; }
        else high=i==(select ? 2u : 3u) ? 86400 : 3600000;
        bad=lmb_json_uint(&j,(unsigned)fields[i],&n) || n<low || n>high;
        if (!bad) snprintf(values[count++],sizeof values[0],"%u",n);
    }
    if (!bad && (measure || select)) {
        unsigned index=(unsigned)fields[measure ? 4 : 3]; const LmbJsonToken *list=&tokens[index];
        bad=list->kind!=LMB_JSON_ARRAY || list->children<(select ? 2u : 1u) || list->children>LMB_CAPACITY_MODELS;
        if (!bad && measure) { uint32_t clients; bad=lmb_json_uint(&j,(unsigned)fields[1],&clients) || list->children>clients; }
        unsigned first=count;
        for (unsigned at=index+1;!bad && at<list->next;at=tokens[at].next) {
            bad=lmb_api_json_string(&j,at,values[count],sizeof values[0]);
            if (!bad && measure) {
                uint8_t id[32],zero[32]={0};
                bad=strlen(values[count])!=64 || lmb_unhex(id,values[count],32) || !memcmp(id,zero,32);
                if (!bad) lmb_hex(values[count],id,32); /* one identity, including hex case */
            } else if (!bad) bad=!lmb_api_username(values[count]);
            for (unsigned k=first;!bad && k<count;k++) if (!strcmp(values[k],values[count])) bad=1;
            count++;
        }
    }
    if (bad) { api_error(fd,400,"invalid_capacity_request"); return; }
    for (unsigned i=0;i<count;i++) args[i]=values[i];
    if (remove_record) {
        int dir=lmb_capacity_dir(access,0),guard=dir<0 ? -1 : lmb_capacity_lock(dir);
        int rc=guard<0 ? -1 : lmb_capacity_remove(dir,values[0]);
        if (guard>=0) close(guard);
        if (dir>=0) close(dir);
        if (rc) api_error(fd,409,"capacity_record_not_removed");
        else (void)api_response(fd,200,"{\"schema\":1,\"removed\":true,\"weights_changed\":false}");
        return;
    }
    Cap result={0}; ApiCapacityObserver observer={.fd=fd};
    uint64_t previous_deadline=lmb_read_deadline_ms;
    /* Reserve the last minute of the HTTP child's lifetime for bounded
     * cancellation/reset and persistence, including stalled handshakes. */
    lmb_read_deadline_ms=lmb_io_monotonic_ms()+240000;
    if (measure) alarm(240);
    int rc=measure ? api_capacity_measure_result(access,tracker,args,count,&result,&observer) :
        check ? api_capacity_check_result(access,tracker,args,count,&result) :
        api_capacity_select_result(access,tracker,args,count,&result);
    lmb_read_deadline_ms=previous_deadline;
    if (observer.started) {
        if (result.p) (void)api_event(fd,"result",result.p);
        else (void)api_event(fd,"error","{\"error\":\"capacity_result_unavailable\"}");
    } else if (result.p) (void)api_response(fd,200,result.p);
    else api_error(fd,rc==2 ? 400 : measure ? 409 : 503,
        rc==2 ? "invalid_capacity_request" : measure ? "capacity_probe_not_started" : "capacity_record_unavailable");
    free(result.p);
}
#endif
