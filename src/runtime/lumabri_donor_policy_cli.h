/* Local donor owner only. Network management credentials cannot create these
 * grants: the authorizing principal is the computer's own OS user. */
#ifndef LMB_DONOR_POLICY_CLI_H
#define LMB_DONOR_POLICY_CLI_H
static int donor_policy_number(const char *s,uint32_t *out) {
    if (!s || !*s) return -1;
    uint64_t n=0;
    for (const char *p=s;*p;p++) {
        if (*p<'0' || *p>'9' || n>UINT32_MAX/10u || n*10u+(unsigned)(*p-'0')>UINT32_MAX) return -1;
        n=n*10u+(unsigned)(*p-'0');
    }
    *out=(uint32_t)n; return 0;
}
static void donor_policy_json(const LmbDonorPolicy *p) {
    char id[65],instance[65],requester[65],root[65],edge[65];
    lmb_hex(id,p->id,32); lmb_hex(instance,p->donor_instance,32); lmb_hex(requester,p->limit.requester,32);
    lmb_hex(root,p->limit.model_root,32); lmb_hex(edge,p->limit.edge_peer,32);
    printf("{\"id\":\"%s\",\"revision\":%u,\"enabled\":%s,\"donor_instance\":\"%s\","
        "\"requester\":\"%s\",\"checkpoint_content_id\":\"%s\",\"edge_peer\":\"%s\",\"tracker\":",id,p->revision,
        p->enabled ? "true" : "false",instance,requester,root,edge);
    json_string(stdout,p->limit.tracker); fputs(",\"adapter_model_type\":",stdout); json_string(stdout,p->limit.model_type);
    printf(",\"begin\":%u,\"end\":%u,\"runs_edge\":%s,\"ram_bytes_per_allocation\":%llu,"
        "\"max_allocations\":%u,\"context\":%u,\"sessions\":%u,\"threads\":%u,\"max_new\":%u,"
        "\"edge_ram_bytes\":%llu,\"metadata_bytes\":%llu,\"checkpoint_bytes\":%llu,\"issued\":%llu,\"expires\":%llu,"
        "\"scope\":\"exact_resident_segment_placement\"}",p->limit.begin,p->limit.end,p->limit.runs_edge ? "true" : "false",
        (unsigned long long)p->limit.ram_bytes,p->allocations,p->limit.context,p->limit.sessions ? p->limit.sessions : 1,
        p->limit.threads,p->limit.max_new,(unsigned long long)p->limit.edge_ram_bytes,
        (unsigned long long)p->limit.disk_bytes,(unsigned long long)p->limit.model_bytes,
        (unsigned long long)p->issued,(unsigned long long)p->expires);
}
static int donor_policy_review(int dir,HomeServiceSnapshot *s,LmbDonorPolicy *p,char review[65],uint32_t *prior) {
    if (home_service_query("donor",HOME_SVC_STATUS,NULL,s) || !s->has_offer || s->phase!=LMB_HOME_PENDING ||
        s->offer.hybrid_role || !home_resident_required()) return -1;
    memset(p,0,sizeof *p); p->limit=s->offer; memcpy(p->donor_instance,s->instance,32);
    if (home_offer_content(&s->offer,p->limit.model_root)) return -1;
    if (lmb_donor_policy_scope(&p->limit,p->id)) return -1;
    LmbDonorPolicy old; int found=lmb_donor_policy_load(dir,p->id,&old);
    if (found<0 || (!found && old.revision==UINT32_MAX)) return -1;
    *prior=found ? 0 : old.revision; p->revision=*prior+1;
    LmbBuf b={0}; uint8_t digest[32];
    int bad=lmb_buf_bytes(&b,s->instance,32) || lmb_buf_u64(&b,s->revision) ||
        lmb_buf_u32(&b,*prior) || lmb_home_offer_pack(&b,&s->offer);
    if (!bad) { lmb_donor_policy_digest(b.p,b.len,digest); lmb_hex(review,digest,32); }
    free(b.p); return bad ? -1 : 0;
}
static int cmd_donor_policy(int argc,char **argv) {
    char path[1200];
    if (argc<1 || home_service_directory(path,sizeof path)) return 1;
    int dir=lmb_donor_policy_dir(path,1); if (dir<0) return home_fail("Cannot open the private donor policy store.");
    int rc=1,lock=-1;
    if (argc==1 && !strcmp(argv[0],"list")) {
        int fd=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC); DIR *scan=fd<0 ? NULL : fdopendir(fd);
        if (!scan) { if (fd>=0) close(fd); goto done; }
        struct dirent *e; unsigned count=0; rc=0; fputs("{\"schema\":1,\"policies\":[",stdout);
        for (;;) {
            errno=0; e=readdir(scan); if (!e) { if (errno) rc=1; break; }
            if (strlen(e->d_name)!=71 || strcmp(e->d_name+64,".policy")) continue;
            char id[65]; memcpy(id,e->d_name,64); id[64]=0; uint8_t raw[32]; LmbDonorPolicy p;
            if (++count>LMB_DONOR_POLICY_MAX || lmb_unhex(raw,id,32) || lmb_donor_policy_load(dir,raw,&p)) { rc=1; break; }
            if (count>1) fputc(',',stdout);
            donor_policy_json(&p);
        }
        fputs("]}\n",stdout); closedir(scan);
    } else if ((argc==1 && !strcmp(argv[0],"inspect")) || (argc==4 && !strcmp(argv[0],"allow-current")) ||
               (argc==2 && !strcmp(argv[0],"accept-current"))) {
        HomeServiceSnapshot s; LmbDonorPolicy p; char review[65]; uint32_t prior;
        lock=lmb_donor_policy_lock(dir);
        if (lock<0 || donor_policy_review(dir,&s,&p,review,&prior)) { home_fail("No live resident Segment offer is available for policy review."); goto done; }
        if (!strcmp(argv[0],"inspect")) {
            p.enabled=0; p.allocations=1;
            printf("{\"schema\":1,\"review\":\"%s\",\"proposed_scope\":",review);
            donor_policy_json(&p); puts("}"); rc=0; goto done;
        }
        if (!strcmp(argv[0],"accept-current")) {
            HomeServiceSnapshot after;
            /* Same immutable offer, keeper and revision as the owner's
             * review. A lost reply is not permission to accept another offer.
             * No repeat grant is saved by this one-off command. */
            if (strcmp(argv[1],review) || home_service_query("donor",HOME_SVC_ACCEPT,&s,&after)) {
                home_fail("Acceptance not confirmed. Inspect the live donor before retrying; the reviewed offer may have changed."); goto done;
            }
            if (after.phase!=LMB_HOME_ACCEPTED) {
                home_fail("Allocation was not accepted: %.400s",after.detail); goto done;
            }
            char allocation[65]; lmb_hex(allocation,s.offer.id,32);
            printf("{\"schema\":1,\"allocation\":\"%s\",\"state\":\"accepted\",\"repeat_permission\":false}\n",allocation);
            rc=0; goto done;
        }
        uint32_t ttl,count;
        if (strcmp(argv[1],review) || donor_policy_number(argv[2],&ttl) || ttl<30 || ttl>604800 ||
            donor_policy_number(argv[3],&count) || !count || count>4) {
            home_fail("Offer/policy changed, or invalid limits. Inspect again; use a 30–604800 second lifetime and 1–4 allocations."); goto done;
        }
        p.enabled=1; p.allocations=count; p.issued=(uint64_t)time(NULL); p.expires=p.issued+ttl;
        if (lmb_donor_policy_store(dir,&p,prior)) { home_fail("Policy save was not confirmed. Inspect before retrying."); goto done; }
        donor_policy_json(&p); putchar('\n'); rc=0;
    } else if (argc==3 && !strcmp(argv[0],"revoke")) {
        uint8_t id[32]; uint32_t expected; LmbDonorPolicy p;
        if (strlen(argv[1])!=64 || lmb_unhex(id,argv[1],32) || donor_policy_number(argv[2],&expected)) goto done;
        lock=lmb_donor_policy_lock(dir);
        if (lock<0 || lmb_donor_policy_load(dir,id,&p) || p.revision!=expected || expected==UINT32_MAX) {
            home_fail("Policy changed or is unavailable. List it again before revoking."); goto done;
        }
        p.enabled=0; p.revision++;
        if (lmb_donor_policy_store(dir,&p,expected)) { home_fail("Revocation not confirmed; inspect the saved policy."); goto done; }
        donor_policy_json(&p); putchar('\n'); rc=0;
    } else fprintf(stderr,"lumabri donor-policy inspect | list | accept-current REVIEW | allow-current REVIEW TTL_SECONDS MAX_ALLOCATIONS | revoke ID REVISION\n");
done:
    if (lock>=0) close(lock);
    close(dir); return rc;
}
#endif
