/* Durable local operator operation. Never infer release from an absent peer.
 * Every write precedes the next external action. Repeating a step uses the
 * same immutable allocation and live-process fences, not a new approval. */
#ifndef LMB_RETIREMENT_H
#define LMB_RETIREMENT_H
_Static_assert(LMB_CLUSTER_MAX_NODES<=32,"retirement release mask must cover every node");
enum { LMB_RET_HOST_DRAIN, LMB_RET_HOST_SEAL, LMB_RET_NODE_DRAIN,
       LMB_RET_NODE_SEAL, LMB_RET_RELEASE, LMB_RET_DONE };
typedef struct {
    uint8_t plan_hash[64];
    uint32_t phase, cursor, count, released;
    LmbNodeControl host, nodes[LMB_CLUSTER_MAX_NODES];
} LmbRetirement;
static int lmb_retirement_valid(const LmbRetirement *r) {
    if (!r->count || r->count>LMB_CLUSTER_MAX_NODES || r->phase>LMB_RET_DONE || r->cursor>r->count ||
        ((uint64_t)r->released>>r->count) || (r->phase<LMB_RET_RELEASE && r->released)) return 0;
    uint8_t nonzero=0; for (unsigned i=0;i<64;i++) nonzero|=r->plan_hash[i];
    if (!nonzero || r->host.sessions>LMB_HOST_MAX_SESSIONS || r->host.experts>r->host.sessions) return 0;
    if (r->phase<=LMB_RET_HOST_SEAL && r->cursor) return 0;
    if (r->phase==LMB_RET_HOST_SEAL && !r->host.draining) return 0;
    for (uint32_t i=0;i<=r->count;i++) {
        const LmbNodeControl *s=i ? &r->nodes[i-1] : &r->host;
        uint8_t bytes[LMB_NODE_CONTROL_REPLY_BYTES]; LmbNodeControl parsed;
        lmb_node_control_reply(bytes,0,s);
        if (lmb_node_control_decode(bytes,sizeof bytes,&parsed)) return 0;
    }
    if (r->phase>=LMB_RET_NODE_DRAIN && r->host.draining!=LMB_NODE_CONTROL_RETIRED) return 0;
    if (r->phase==LMB_RET_NODE_DRAIN) for (uint32_t i=0;i<r->cursor;i++)
        if (!r->nodes[i].draining) return 0;
    if (r->phase==LMB_RET_NODE_SEAL) for (uint32_t i=0;i<r->count;i++)
        if (!r->nodes[i].draining || (i<r->cursor && r->nodes[i].draining!=LMB_NODE_CONTROL_RETIRED)) return 0;
    if (r->phase>=LMB_RET_RELEASE) for (uint32_t i=0;i<r->count;i++)
        if (r->nodes[i].draining!=LMB_NODE_CONTROL_RETIRED) return 0;
    if (r->phase>=LMB_RET_RELEASE) {
        unsigned completed=0;
        for (uint32_t bits=r->released;bits;bits>>=1) completed+=bits&1u;
        if (completed!=r->cursor) return 0;
    }
    return r->phase!=LMB_RET_DONE || (uint64_t)r->released==((UINT64_C(1)<<r->count)-1);
}
static int lmb_retirement_encode(const LmbRetirement *r, LmbBuf *b) {
    if (!lmb_retirement_valid(r) || lmb_buf_reserve(b,4096)) return -1;
    int bad=lmb_buf_bytes(b,"LMBRET1\0",8) || lmb_buf_bytes(b,r->plan_hash,64) ||
        lmb_buf_u32(b,r->phase) || lmb_buf_u32(b,r->cursor) || lmb_buf_u32(b,r->count) || lmb_buf_u32(b,r->released);
    for (uint32_t i=0;!bad && i<=r->count;i++) {
        uint8_t bytes[LMB_NODE_CONTROL_REPLY_BYTES]; lmb_node_control_reply(bytes,0,i ? &r->nodes[i-1] : &r->host);
        bad=lmb_buf_bytes(b,bytes,sizeof bytes);
    }
    return bad ? -1 : 0;
}
static int lmb_retirement_decode(const uint8_t *bytes, size_t size, LmbRetirement *r) {
    memset(r,0,sizeof *r);
    if (size<88 || memcmp(bytes,"LMBRET1\0",8)) return -1;
    LmbCur c={bytes,size,8};
    if (lmb_cur_bytes(&c,r->plan_hash,64) || lmb_cur_u32(&c,&r->phase) || lmb_cur_u32(&c,&r->cursor) ||
        lmb_cur_u32(&c,&r->count) || lmb_cur_u32(&c,&r->released) || !r->count || r->count>LMB_CLUSTER_MAX_NODES ||
        c.len-c.off!=(r->count+1)*LMB_NODE_CONTROL_REPLY_BYTES) return -1;
    for (uint32_t i=0;i<=r->count;i++,c.off+=LMB_NODE_CONTROL_REPLY_BYTES)
        if (lmb_node_control_decode(c.p+c.off,LMB_NODE_CONTROL_REPLY_BYTES,i ? &r->nodes[i-1] : &r->host)) return -1;
    return lmb_retirement_valid(r) ? 0 : -1;
}
static int lmb_retirement_dir(int access) {
    if (mkdirat(access,"retirements",0700) && errno!=EEXIST) return -1;
    int fd=openat(access,"retirements",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd>=0 && lmb_api_private_fd(fd,1)) { close(fd); return -1; }
    return fd;
}
static int lmb_retirement_load(int dir, const char *name, LmbRetirement *r) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    struct stat s; uint8_t bytes[4096];
    int bad=lmb_api_private_fd(fd,0) || fstat(fd,&s) || s.st_size<88 || s.st_size>(off_t)sizeof bytes;
    if (!bad) bad=lmb_read_full(fd,bytes,(size_t)s.st_size) || lmb_retirement_decode(bytes,(size_t)s.st_size,r);
    close(fd); return bad ? -1 : 0;
}
static int lmb_retirement_store(int dir, const char *name, const LmbRetirement *r) {
    LmbBuf bytes={0}; if (lmb_retirement_encode(r,&bytes)) { free(bytes.p); return -1; }
    /* One private directory lock serializes all retirement writers. Fixed
     * transaction name bounds crash debris. Unsafe existing entries fail. */
    struct stat s; int bad=0;
    if (!fstatat(dir,"pending",&s,AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(s.st_mode) || s.st_uid!=geteuid() || (s.st_mode&077) || s.st_nlink!=1 || unlinkat(dir,"pending",0)) bad=1;
    } else if (errno!=ENOENT) bad=1;
    int fd=bad ? -1 : openat(dir,"pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) bad=1;
    for (size_t at=0;!bad && at<bytes.len;) {
        ssize_t n=write(fd,bytes.p+at,bytes.len-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) bad=1; else at+=(size_t)n;
    }
    free(bytes.p);
    if (fd>=0) { if (!bad && fsync(fd)) bad=1; if (close(fd)) bad=1; }
    if (!bad && renameat(dir,"pending",dir,name)) bad=1;
    if (!bad && fsync(dir)) bad=1;
    return bad ? -1 : 0;
}
static int lmb_retirement_room(int dir) {
    int copy=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC); if (copy<0) return -1;
    DIR *entries=fdopendir(copy); if (!entries) { close(copy); return -1; }
    struct dirent *entry; unsigned count=0;
    while ((entry=readdir(entries))) if (strlen(entry->d_name)==71 && !strcmp(entry->d_name+64,".retire")) count++;
    closedir(entries); return count<256 ? 0 : -1;
}
static int lmb_retirement_plan_hash(const LmbResidentPlan *p, uint8_t out[64]) {
    LmbBuf b={0};
    int bad=lmb_buf_bytes(&b,p->allocation,32) || lmb_buf_str(&b,p->root) || lmb_buf_str(&b,p->tracker) ||
        lmb_buf_str(&b,p->host) || lmb_buf_str(&b,p->host_key) || lmb_buf_u32(&b,p->execution.count);
    for (uint32_t i=0;!bad && i<p->execution.count;i++) {
        const LmbExecutionNode *n=&p->execution.nodes[i];
        bad=lmb_buf_bytes(&b,p->peer_keys[i],32) || lmb_buf_str(&b,n->address) || lmb_buf_u32(&b,n->begin) ||
            lmb_buf_u32(&b,n->end) || lmb_buf_u32(&b,(uint32_t)n->edge);
    }
    if (!bad) lmb_sha512(b.p,b.len,out);
    free(b.p); return bad ? -1 : 0;
}
static LmbNodeControl lmb_retirement_host_state(const LmbHostControl *h) {
    LmbNodeControl n={.revision=h->revision,.draining=h->draining,.sessions=h->connections,.experts=h->requests};
    memcpy(n.instance,h->instance,32); return n;
}
/* Returns 1 when admitted work still exists, -1 on unknown/conflict. The
 * caller retains its journal and never rolls accepted mutations back. */
static int lmb_retirement_host_step(const LmbResidentPlan *p, LmbRetirement *r, uint32_t action) {
    LmbHostControl expected={.revision=r->host.revision,.draining=r->host.draining}, actual={0};
    memcpy(expected.instance,r->host.instance,32);
    int status=lmb_host_control_rpc(p->host,p->host_key,p->root,action,&expected,&actual);
    if (status==LMB_HOST_CONTROL_CONFLICT && action==LMB_HOST_CONTROL_RETIRE &&
        !memcmp(actual.instance,expected.instance,32) && actual.revision==expected.revision && actual.draining==1 &&
        (actual.connections || actual.requests)) return 1;
    if (status) return -1;
    r->host=lmb_retirement_host_state(&actual); return 0;
}
static int lmb_retirement_run(int dir, const char *name, const LmbResidentPlan *p, LmbRetirement *r) {
    /* A valid record also has to match the plan's release order. A cursor
     * must never skip a node whose acknowledgement is absent. */
    if (!lmb_retirement_valid(r) || r->count!=p->execution.count) return -1;
    uint32_t order[LMB_CLUSTER_MAX_NODES],at=0,mask=0;
    for (uint32_t i=0;i<r->count;i++) if (!p->execution.nodes[i].edge) order[at++]=i;
    for (uint32_t i=0;i<r->count;i++) if (p->execution.nodes[i].edge) order[at++]=i;
    if (r->phase>=LMB_RET_RELEASE) {
        for (uint32_t i=0;i<r->cursor;i++) mask|=UINT32_C(1)<<order[i];
        if (r->released!=mask) return -1;
    }
    while (r->phase!=LMB_RET_DONE) {
        int status=0;
        if (r->phase==LMB_RET_HOST_DRAIN) {
            if (!r->host.draining) status=lmb_retirement_host_step(p,r,LMB_HOST_CONTROL_DRAIN);
            if (!status) r->phase=LMB_RET_HOST_SEAL;
        } else if (r->phase==LMB_RET_HOST_SEAL) {
            if (r->host.draining!=LMB_HOST_CONTROL_RETIRED) status=lmb_retirement_host_step(p,r,LMB_HOST_CONTROL_RETIRE);
            if (!status) { r->phase=LMB_RET_NODE_DRAIN; r->cursor=0; }
        } else if (r->phase==LMB_RET_NODE_DRAIN || r->phase==LMB_RET_NODE_SEAL) {
            if (r->cursor==r->count) { r->phase++; r->cursor=0; }
            else {
                uint32_t i=r->cursor, action=r->phase==LMB_RET_NODE_DRAIN ? LMB_NODE_CONTROL_DRAIN : LMB_NODE_CONTROL_RETIRE;
                LmbNodeControl *expected=&r->nodes[i],actual={0};
                if ((action==LMB_NODE_CONTROL_DRAIN && !expected->draining) ||
                    (action==LMB_NODE_CONTROL_RETIRE && expected->draining!=LMB_NODE_CONTROL_RETIRED)) {
                    status=home_resident_node_control(p,i,action,expected,&actual);
                    if (status==LMB_NODE_CONTROL_CONFLICT && action==LMB_NODE_CONTROL_RETIRE &&
                        !memcmp(actual.instance,expected->instance,32) && actual.revision==expected->revision &&
                        actual.draining==1 && (actual.sessions || actual.experts)) return 1;
                    if (!status) *expected=actual; else return -1;
                }
                if (!status) r->cursor++;
            }
        } else if (r->phase==LMB_RET_RELEASE) {
            /* Release the Edge keeper last, after every other node confirms
             * the exact sealed fence. No ordinary forced RELEASE is used. */
            if (r->cursor==r->count) r->phase=LMB_RET_DONE;
            else {
                uint32_t i=order[r->cursor];
                status=home_resident_retired_release(p,i,&r->nodes[i]);
                if (!status) { r->released|=UINT32_C(1)<<i; r->cursor++; }
            }
        } else return -1;
        if (status) return status;
        if (lmb_retirement_store(dir,name,r)) return -1;
    }
    return 0;
}
/* Shared service operation. A browser starts with a live host fence; the
 * persisted operation then owns its subsequent fences, including retries
 * after a lost response. A stale first request cannot retire a replaced host. */
static int api_retirement_execute(int access, const char *tracker, const uint8_t id[32],
    const LmbHostControl *expected, LmbRetirement *result, int *conflict) {
    uint8_t hash[64]; char hex[65],name[80]; *conflict=0;
    lmb_hex(hex,id,32); snprintf(name,sizeof name,"%s.retire",hex);
    LmbResidentPlan *p=calloc(1,sizeof *p); LmbRetirement r={0};
    if (!p) return -1;
    int dir=lmb_retirement_dir(access),lock=dir<0 ? -1 : lmb_api_access_lock(dir),status=-1;
    if (lock<0 || api_find_plan(tracker,id,p) || lmb_retirement_plan_hash(p,hash)) goto done;
    int loaded=lmb_retirement_load(dir,name,&r);
    if (loaded<0) goto done;
    if (loaded==1) {
        if (lmb_retirement_room(dir)) goto done;
        memcpy(r.plan_hash,hash,64); r.count=p->execution.count;
        LmbHostControl host={0};
        if (lmb_host_control_rpc(p->host,p->host_key,p->root,0,NULL,&host)) goto done;
        if (expected && (memcmp(host.instance,expected->instance,32) || host.revision!=expected->revision)) {
            *conflict=1; goto done;
        }
        r.host=lmb_retirement_host_state(&host);
        for (uint32_t i=0;i<r.count;i++) if (home_resident_node_control(p,i,0,NULL,&r.nodes[i])) goto done;
        if (lmb_retirement_store(dir,name,&r)) goto done;
    }
    if (memcmp(r.plan_hash,hash,64) || r.count!=p->execution.count) goto done;
    if (expected && (memcmp(r.host.instance,expected->instance,32) || expected->revision>r.host.revision)) {
        *conflict=1; goto done;
    }
    status=lmb_retirement_run(dir,name,p,&r);
done:
    if (lock>=0) close(lock);
    if (dir>=0) close(dir);
    free(p);
    *result=r; return status;
}
static int api_retire(int access, const char *tracker, char *const *args, unsigned count) {
    uint8_t id[32]; char hex[65];
    if (count!=1 || !*tracker || strlen(args[0])!=64 || lmb_unhex(id,args[0],32)) return 1;
    lmb_hex(hex,id,32); LmbRetirement r={0}; int conflict=0;
    int status=api_retirement_execute(access,tracker,id,NULL,&r,&conflict);
    printf("{\"schema\":1,\"allocation\":\"%s\",\"state\":\"%s\",\"phase\":%u,\"released_node_mask\":%u,\"complete\":%s}\n",
        hex,status<0 ? "unknown_or_conflict" : status ? "waiting_for_admitted_work" : "released",r.phase,r.released,
        !status && r.phase==LMB_RET_DONE ? "true" : "false");
    if (status) fprintf(stderr,"Retirement is incomplete. Accepted drains/seals remain in effect; rerun this exact operation to reconcile. Unknown peers are never assumed released.\n");
    return status<0 ? 1 : 0;
}
#endif
