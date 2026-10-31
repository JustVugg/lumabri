/* Native household keeper. Approval uses the common private service fence;
 * network identity grants a request, never permission to allocate memory.
 * Each slot owns its worker job, listener and immutable lifetime evidence. */
#ifndef LMB_DONOR_WINDOWS_H
#define LMB_DONOR_WINDOWS_H
#include "src/runtime/lumabri_service.h"
#include "src/runtime/lumabri_allocation_windows.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include "lumabri_home_net.h"
#include "lumabri_secure.h"
#include "lumabri_machine.h"
#include "lumabri_sha.h"
#include "lumabri_runtime_probe.h"
#include "src/runtime/lumabri_inventory_windows.h"
#define LMB_COMPUTE_BROKER_SERVER
#include "src/runtime/lumabri_compute_broker.h"

typedef struct {
    LmbHomeTransaction transaction;
    LmbOwnedProcess worker;
    LmbSocket client,listener;
    HANDLE ready,control,preflight,handoff,lifetime;
    uint32_t port;
    int retained,release_pending,birth_started;
    LmbHomePhase release_phase;
    char release_reason[160];
} LmbWindowsDonorSlot;

typedef struct {
    HomeService service;
    LmbWindowsInventory inventory;
    int inventory_started;uint64_t next_inventory;
    char runtime_id[65];
    LmbWindowsDonorSlot slots[4];
    LmbMachineProfile machine;
    LmbComputeBroker compute;
    LmbSocket listener;
    HANDLE executable;
    int lease,opened,stopping;
    unsigned selected,thread_capacity;
    uint64_t budget,reserve;
    char worker[1600],tracker[256],ip[64],control[64],compute_path[1400];
} LmbWindowsDonor;

static void lmb_donor_handle_close(HANDLE *h) {
    if (*h && *h!=INVALID_HANDLE_VALUE) CloseHandle(*h);
    *h=INVALID_HANDLE_VALUE;
}
static int lmb_donor_status_windows(LmbSocket fd,const LmbWindowsDonorSlot *slot) {
    const LmbHomeTransaction *t=&slot->transaction; LmbBuf b={0};
    int bad=lmb_buf_u32(&b,LMB_HOME_VERSION) || lmb_buf_bytes(&b,t->offer.id,32) ||
        lmb_buf_u32(&b,t->phase) || lmb_buf_u32(&b,slot->port) || lmb_buf_u32(&b,0) ||
        lmb_buf_str(&b,t->reason) || lmb_send(fd,LMB_HOME_STATUS,b.p,(uint32_t)b.len,NULL,0);
    free(b.p); return bad ? -1 : 0;
}
static uint64_t lmb_donor_reserved_windows(const LmbWindowsDonor *d) {
    uint64_t used=0;
    for (unsigned i=0;i<4;i++) if (d->slots[i].transaction.reservation_held)
        used+=d->slots[i].transaction.offer.ram_bytes;
    return used;
}
static uint64_t lmb_donor_room_windows(LmbWindowsDonor *d) {
    lmb_machine_refresh_resources(&d->machine,d->service.directory);
    uint64_t available=d->machine.ram_available_bytes> d->reserve ? d->machine.ram_available_bytes-d->reserve : 0;
    uint64_t used=lmb_donor_reserved_windows(d),loading=0;
    for (unsigned i=0;i<4;i++) if (d->slots[i].transaction.reservation_held && !d->slots[i].retained)
        loading+=d->slots[i].transaction.offer.ram_bytes;
    available=available>loading ? available-loading : 0;
    uint64_t room=used<d->budget ? d->budget-used : 0;
    return room<available ? room : available;
}
static void lmb_donor_snapshot_windows(LmbWindowsDonor *d) {
    HomeServiceSnapshot *s=&d->service.snapshot;
    LmbWindowsDonorSlot *slot=&d->slots[d->selected];
    s->phase=slot->transaction.phase;s->offer=slot->transaction.offer;
    s->has_offer=lmb_home_offer_valid(&s->offer);s->retained=slot->retained;
    s->threads=d->thread_capacity;s->segment_pid=slot->worker.pid;s->ram=d->budget;
    s->reserved_total=lmb_donor_reserved_windows(d);s->model_count=0;
    s->compute_enabled=d->compute.started && !atomic_load(&d->compute.stop);
    s->compute_active=s->compute_enabled ? atomic_load(&d->compute.active) : 0;
    s->compute_queued=s->compute_enabled ? atomic_load(&d->compute.queued) : 0;
    s->compute_grants=atomic_load(&d->compute.grants);
    snprintf(s->tracker,sizeof s->tracker,"%s",d->tracker);
    snprintf(s->detail,sizeof s->detail,"%s",slot->transaction.reason);
    uint8_t ids[4][32];
    for (unsigned i=0;i<4;i++) {
        const LmbHomeTransaction *t=&d->slots[i].transaction;
        if (!t->reservation_held && t->phase!=LMB_HOME_PENDING) continue;
        unsigned at=s->model_count++;
        while (at && memcmp(ids[at-1],t->offer.id,32)>0) {memcpy(ids[at],ids[at-1],32);at--;}
        memcpy(ids[at],t->offer.id,32);
    }
    LmbSha hash;lmb_sha_init(&hash);
    lmb_sha_update(&hash,"lumabri-allocation-set-v1",sizeof "lumabri-allocation-set-v1");
    lmb_sha_update(&hash,ids,s->model_count*32);lmb_sha_final(&hash,s->allocation_set);
}
static void lmb_donor_inventory_windows(LmbWindowsDonor *d,LmbMachineReport *r) {
    memset(r,0,sizeof *r);
    r->ram_budget_bytes=lmb_donor_room_windows(d);r->machine=d->machine;
    snprintf(r->control_addr,sizeof r->control_addr,"%s",d->control);
    snprintf(r->runtime_id,sizeof r->runtime_id,"%s",d->runtime_id);
    r->runtime_threads=d->thread_capacity;r->roles.known=1;
    /* These are the keeper's accepted roles, not every adapter in Colibri.
     * Edge and Hybrid are explicitly unavailable in this native build. */
    strcpy(r->roles.segment,"olmoe");r->facts=lmb_resource_facts_local(d->machine.load_one);
    HomeServiceSnapshot *s=&d->service.snapshot;
    r->workload=(LmbWorkloadFacts){.known=1,.allocations=s->model_count,.reserved_bytes=s->reserved_total,
        .compute_policy=s->compute_enabled ? LMB_COMPUTE_LOCAL_FIFO : LMB_COMPUTE_UNKNOWN,
        .active=s->compute_active,.queued=s->compute_queued};
    memcpy(r->workload.allocation_set,s->allocation_set,32);
}
static int lmb_donor_runtime_id_windows(LmbWindowsDonor *d) {
    LmbSha hash;lmb_sha_init(&hash);uint8_t block[65536],digest[32];DWORD n;
    LARGE_INTEGER zero={0};if (!SetFilePointerEx(d->executable,zero,NULL,FILE_BEGIN)) return -1;
    lmb_sha_update(&hash,"lumabri-native-donor-cpu-olmoe-v1",sizeof "lumabri-native-donor-cpu-olmoe-v1");
    lmb_sha_update(&hash,d->service.snapshot.instance,32);
    for (;;) {
        if (!ReadFile(d->executable,block,sizeof block,&n,NULL)) return -1;
        if (!n) break;
        lmb_sha_update(&hash,block,n);
    }
    lmb_sha_final(&hash,digest);lmb_hex(d->runtime_id,digest,32);return 0;
}
static int lmb_donor_release_windows(LmbWindowsDonor *d,LmbWindowsDonorSlot *s,
        LmbHomePhase phase,const char *reason) {
    s->retained=0;
    if (!s->release_pending) {
        s->release_phase=phase;snprintf(s->release_reason,sizeof s->release_reason,"%s",reason);
    }
    s->release_pending=1;
    if (s->worker.process && (lmb_owned_process_stop(&s->worker,0,1000) || lmb_owned_process_close(&s->worker))) {
        s->transaction.phase=LMB_HOME_FAILED;
        snprintf(s->transaction.reason,sizeof s->transaction.reason,"Waiting for the owned worker to exit; memory remains reserved.");
        return -1;
    }
    lmb_donor_handle_close(&s->lifetime);
    if (s->birth_started) {
        LmbAllocationLifetime proof;
        if (lmb_allocation_prove_windows(d->service.directory,s->transaction.offer.id,&proof)) {
            snprintf(s->transaction.reason,sizeof s->transaction.reason,"Waiting for final handle release; memory remains reserved (errno=%d, Windows=%lu).",errno,(unsigned long)GetLastError());return -1;
        }
    }
    lmb_donor_handle_close(&s->ready);lmb_donor_handle_close(&s->control);
    lmb_donor_handle_close(&s->preflight);lmb_donor_handle_close(&s->handoff);
    if (s->listener!=LMB_INVALID_SOCKET) lmb_socket_close(s->listener);
    s->listener=LMB_INVALID_SOCKET;s->port=0;s->release_pending=0;s->birth_started=0;
    lmb_home_released(&s->transaction,s->release_phase,s->release_reason);
    if (s->client!=LMB_INVALID_SOCKET) {
        (void)lmb_donor_status_windows(s->client,s);lmb_close(s->client);s->client=LMB_INVALID_SOCKET;
    }
    return 0;
}
static void lmb_donor_lost_windows(LmbWindowsDonor *d,LmbWindowsDonorSlot *s) {
    if (s->client!=LMB_INVALID_SOCKET) lmb_close(s->client);
    s->client=LMB_INVALID_SOCKET;
    if (s->retained && s->worker.process && WaitForSingleObject(s->worker.process,0)==WAIT_TIMEOUT) {
        snprintf(s->transaction.reason,sizeof s->transaction.reason,"Chat closed. Weights stay in RAM until this computer stops sharing.");
    } else (void)lmb_donor_release_windows(d,s,LMB_HOME_CLOSED,"Requester disconnected before preparation completed.");
}

static int lmb_donor_launch_windows(LmbWindowsDonor *d,LmbWindowsDonorSlot *s) {
    const LmbHomeOffer *o=&s->transaction.offer;
    LmbAllocationLifetime birth={.reserved_bytes=o->ram_bytes,.begin=o->begin,.end=o->end};
    memcpy(birth.allocation,o->id,32);memcpy(birth.root,o->model_root,32);memcpy(birth.requester,o->requester,32);
    LmbBuf packed={0};if (lmb_home_offer_pack(&packed,o)) {free(packed.p);return -1;}
    lmb_sha512(packed.p,packed.len,birth.offer_hash);free(packed.p);
    s->lifetime=lmb_allocation_create_windows(d->service.directory,&birth);
    if (s->lifetime==INVALID_HANDLE_VALUE) return -1;
    s->birth_started=1;
    char id[65],root[65],signer[65],peer[100],folder[1600],cache[1440],log_path[1440];
    lmb_hex(id,o->id,32);lmb_hex(root,o->model_root,32);lmb_hex(signer,o->requester,32);
    snprintf(peer,sizeof peer,"LUMABRI_SEGMENT_CLIENT_PK=");lmb_hex(peer+strlen(peer),o->edge_peer,32);
    if (snprintf(cache,sizeof cache,"%s/checkpoints",d->service.directory)>=(int)sizeof cache ||
        lmb_private_mkdir(cache) || snprintf(folder,sizeof folder,"%s/%s",cache,id)>=(int)sizeof folder ||
        snprintf(log_path,sizeof log_path,"%s/engines.log",d->service.directory)>=(int)sizeof log_path) return -1;
    HANDLE log=lmb_private_open(log_path,1,1,NULL);if (log==INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER end={0};if (!SetFilePointerEx(log,end,NULL,FILE_END)) {CloseHandle(log);return -1;}
    int port=0;s->listener=lmb_home_listen_service(&port);s->port=(uint32_t)port;
    HANDLE handoff[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
    HANDLE control[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE},preflight[2]={INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};
    int rc=-1;
    if (s->listener==LMB_INVALID_SOCKET || lmb_pipe_pair(handoff) || lmb_pipe_pair(control) || lmb_pipe_pair(preflight)) goto done;
    s->ready=CreateEventW(NULL,TRUE,FALSE,NULL);if (!s->ready) goto done;
    char range[40],port_text[16],address[100],name[64],ctx[16],ram[32],bytes[32],layers[16],sessions[16];
    char threads[16],compute[1440],reserve[96];
    snprintf(range,sizeof range,"%u:%u",o->begin,o->end);snprintf(port_text,sizeof port_text,"%d",port);
    snprintf(address,sizeof address,"%s:%d",d->ip,port);snprintf(name,sizeof name,"home-%.12s-%u",id,o->begin);
    snprintf(ctx,sizeof ctx,"%u",o->context);snprintf(ram,sizeof ram,"%llu",(unsigned long long)(o->ram_bytes>>20));
    snprintf(bytes,sizeof bytes,"%llu",(unsigned long long)o->model_bytes);snprintf(layers,sizeof layers,"%u",o->layers);
    snprintf(sessions,sizeof sessions,"%u",o->sessions ? o->sessions : 1);snprintf(threads,sizeof threads,"%u",o->threads);
    snprintf(compute,sizeof compute,"LUMABRI_COMPUTE_SOCKET=%s",d->compute_path);
    snprintf(reserve,sizeof reserve,"LUMABRI_SEGMENT_RAM_RESERVE_MB=%llu",(unsigned long long)(d->reserve>>20));
    const char *args[]={"--engine","olmoe","--model-dir",folder,"--model",o->model,"--range",range,
        "--port",port_text,"--tracker",o->tracker,"--advertise",address,"--name",name,"--model-root",root,
        "--tokenizer-root",root,"--context",ctx,"--max-rows","16","--sessions",sessions,"--threads",threads,
        "--memory-limit-mb",ram,"--checkpoint-signer",signer,"--model-bytes",bytes,"--model-layers",layers};
    const char *environment[]={peer,compute,reserve,"LUMABRI_RESIDENT_REQUIRED=1","LUMABRI_RESIDENCY_POLICY=engine-locked",
        "LUMABRI_ENGINE_BACKEND=cpu","LUMABRI_ENCRYPT=1","PIN=off","PILOT=0","HOT=0","COLI_NO_OMP_TUNE=1"};
    LmbChildHandle handles[]={{"LUMABRI_HOME_LISTEN_HANDLE",handoff[1]},
        {"LUMABRI_NODE_CONTROL_HANDLE",control[1]},{"LUMABRI_PREFLIGHT_HANDLE",preflight[1]},
        {"LUMABRI_READY_HANDLE",s->ready},{"LUMABRI_ALLOCATION_LIFETIME_HANDLE",s->lifetime},
        {"LUMABRI_MACHINE_LEASE_HANDLE",(HANDLE)_get_osfhandle(d->lease)}};
    LmbProcessLaunch launch={.binary=d->worker,.args=args,.nargs=sizeof args/sizeof *args,
        .environment=environment,.nenvironment=sizeof environment/sizeof *environment,
        .handles=handles,.nhandles=sizeof handles/sizeof *handles,.output=log,.error=log};
    if (lmb_owned_process_start(&s->worker,&launch) ||
        lmb_home_export_listener(s->listener,s->worker.pid,handoff[0])) goto done;
    s->handoff=handoff[0];handoff[0]=INVALID_HANDLE_VALUE;
    s->control=control[0];control[0]=INVALID_HANDLE_VALUE;
    s->preflight=preflight[0];preflight[0]=INVALID_HANDLE_VALUE;rc=0;
 done:
    for (unsigned i=0;i<2;i++) {lmb_donor_handle_close(&handoff[i]);lmb_donor_handle_close(&control[i]);lmb_donor_handle_close(&preflight[i]);}
    CloseHandle(log);return rc;
}

static int lmb_donor_authorize_windows(LmbSocket fd,LmbMsg *m) {
    if (lmb_socket_timeout(fd,1000) || lmb_secure_server(fd)) return -1;
    lmb_set_io_timeout(fd,1000);
    if (lmb_recv_bounded(fd,m,16384,1000)) return -1;
    const char *token=getenv("LUMABRI_TOKEN");
    if (token && *token) {
        char actual[LMB_TOKEN_MAX+1]="",expected[LMB_TOKEN_MAX+1]="";LmbCur c={m->body,m->body_len,0};
        int bad=m->op!=LMB_AUTH || m->pay_len || strlen(token)>LMB_TOKEN_MAX ||
            lmb_cur_str(&c,actual,sizeof actual) || c.off!=c.len;
        if (!bad) {snprintf(expected,sizeof expected,"%s",token);bad=!lmb_token_equal(actual,expected);}
        lmb_msg_free(m);
        if (bad || lmb_send(fd,LMB_OK,NULL,0,NULL,0) || lmb_recv_bounded(fd,m,16384,1000)) return -1;
    }
    return 0;
}
static void lmb_donor_exit_proof_windows(LmbWindowsDonor *d,LmbSocket fd,const uint8_t *body) {
    LmbAllocationLifetime r;uint8_t bytes[LMB_LIFETIME_BYTES];
    if (!lmb_allocation_prove_windows(d->service.directory,body,&r) && !memcmp(body+32,r.root,32) &&
        lmb_secure_peer_matches(fd,r.requester) && !lmb_lifetime_encode(&r,bytes))
        (void)lmb_send(fd,LMB_HOME_EXIT_PROOF_R,bytes,sizeof bytes,NULL,0);
}
static void lmb_donor_retire_windows(LmbWindowsDonor *d,LmbWindowsDonorSlot *s,LmbSocket fd,const uint8_t *body) {
    const uint8_t *query=body+64;LmbRetiredReceipt receipt;uint8_t reply[64+LMB_NODE_CONTROL_REPLY_BYTES];
    if (lmb_get32(query)!=LMB_NODE_CONTROL_VERSION || lmb_get32(query+4)!=LMB_NODE_CONTROL_QUERY) return;
    int found=lmb_receipt_read_windows(d->service.directory,body,1,&receipt);
    if (found<0) return;
    if (found) {
        /* A crash or asynchronous handle teardown can occur after the intent
         * but before DONE. Complete only from the same immutable intent and
         * a fresh last-holder exit proof, never from an absent PID. */
        LmbRetiredReceipt intent;
        int pending=lmb_receipt_read_windows(d->service.directory,body,0,&intent);
        if (pending<0) return;
        if (!pending) {
            if (memcmp(body+32,intent.root,32) || !lmb_secure_peer_matches(fd,intent.requester) ||
                memcmp(query+8,intent.seal.instance,32) || lmb_node_control_u64(query+40)!=intent.seal.revision) return;
            if (s && s->transaction.reservation_held &&
                lmb_donor_release_windows(d,s,LMB_HOME_CLOSED,"Retired after fenced idle admission.")) return;
            intent.phase=LMB_RECEIPT_DONE;
            if (lmb_receipt_store_windows(d->service.directory,&intent)) return;
            receipt=intent;found=0;
        }
    }
    if (found) {
        if (!s || !s->retained || s->control==INVALID_HANDLE_VALUE) return;
        uint8_t request[LMB_NODE_CONTROL_REQUEST_BYTES];LmbNodeControl state;
        lmb_node_control_request(request,LMB_NODE_CONTROL_QUERY,NULL);
        if (lmb_node_control_local(&s->control,request,reply+64) ||
            lmb_node_control_decode(reply+64,LMB_NODE_CONTROL_REPLY_BYTES,&state) ||
            state.draining!=LMB_NODE_CONTROL_RETIRED || state.sessions || state.experts ||
            memcmp(state.instance,query+8,32) || state.revision!=lmb_node_control_u64(query+40)) return;
        receipt=(LmbRetiredReceipt){.phase=LMB_RECEIPT_PREPARED,.seal=state};
        memcpy(receipt.allocation,body,32);memcpy(receipt.root,body+32,32);
        memcpy(receipt.requester,s->transaction.offer.requester,32);
        if (lmb_receipt_store_windows(d->service.directory,&receipt) ||
            lmb_donor_release_windows(d,s,LMB_HOME_CLOSED,"Retired after fenced idle admission.")) return;
        receipt.phase=LMB_RECEIPT_DONE;
        if (lmb_receipt_store_windows(d->service.directory,&receipt)) {fprintf(stderr,"[donor] cannot persist terminal receipt: errno=%d win32=%lu\n",errno,(unsigned long)GetLastError());return;}
    }
    if (memcmp(body+32,receipt.root,32) || !lmb_secure_peer_matches(fd,receipt.requester) ||
        memcmp(query+8,receipt.seal.instance,32) || lmb_node_control_u64(query+40)!=receipt.seal.revision ||
        lmb_receipt_store_windows(d->service.directory,&receipt)) return;
    memcpy(reply,body,64);lmb_node_control_reply(reply+64,0,&receipt.seal);
    (void)lmb_send(fd,LMB_HOME_RETIRED_RELEASE_R,reply,sizeof reply,NULL,0);
}
static void lmb_donor_incoming_windows(LmbWindowsDonor *d,LmbSocket fd) {
    LmbMsg m={0};LmbHomeOffer offer={0};LmbWindowsDonorSlot *target=NULL;
    if (lmb_donor_authorize_windows(fd,&m) || m.pay_len) goto done;
    if (m.op==LMB_HOME_FEATURES && !m.body_len) {
        uint8_t version[4];lmb_put32(version,1);(void)lmb_send(fd,LMB_OK,version,4,NULL,0);goto done;
    }
    if (m.op==LMB_HOME_EXIT_PROOF && m.body_len==64) {lmb_donor_exit_proof_windows(d,fd,m.body);goto done;}
    if (m.op==LMB_HOME_QUERY || m.op==LMB_HOME_RELEASE || m.op==LMB_HOME_NODE_CONTROL || m.op==LMB_HOME_RETIRED_RELEASE) {
        unsigned expected=(m.op==LMB_HOME_NODE_CONTROL || m.op==LMB_HOME_RETIRED_RELEASE) ? 64+LMB_NODE_CONTROL_REQUEST_BYTES : 64;
        if (m.body_len!=expected) goto done;
        for (unsigned i=0;i<4;i++) if (!memcmp(m.body,d->slots[i].transaction.offer.id,32)) target=&d->slots[i];
        if (target && (memcmp(m.body+32,target->transaction.offer.model_root,32) ||
            !lmb_secure_peer_matches(fd,target->transaction.offer.requester))) goto done;
        if (m.op==LMB_HOME_RETIRED_RELEASE) {lmb_donor_retire_windows(d,target,fd,m.body);goto done;}
        if (!target) goto done;
        if (m.op==LMB_HOME_NODE_CONTROL) {
            uint8_t response[64+LMB_NODE_CONTROL_REPLY_BYTES];
            if (target->retained && !lmb_node_control_local(&target->control,m.body+64,response+64)) {
                memcpy(response,m.body,64);(void)lmb_send(fd,LMB_HOME_NODE_CONTROL_R,response,sizeof response,NULL,0);
            }
        } else {
            if (m.op==LMB_HOME_RELEASE) (void)lmb_donor_release_windows(d,target,LMB_HOME_CLOSED,"Released by the original requester.");
            (void)lmb_donor_status_windows(fd,target);
        }
        goto done;
    }
    LmbCur c={m.body,m.body_len,0};
    if (m.op!=LMB_HOME_OFFER || lmb_home_offer_unpack(&c,&offer) ||
        !lmb_secure_peer_matches(fd,offer.requester)) goto done;
    const char *reason=NULL;const LmbModelFamily *family=lmb_family_for(offer.model_type);
    if (!family || strcmp(family->segment_id,"olmoe") || offer.runs_edge || offer.hybrid_role || offer.edge_ram_bytes)
        reason="This native runtime provides resident OLMoE Segment only; Edge/Hybrid are unavailable.";
    else if (offer.threads>d->thread_capacity) reason="Requested threads exceed the installed runtime capacity.";
    else if (strcmp(offer.tracker,d->tracker)) reason="Household mismatch; no resources were shared.";
    else if (lmb_allocation_unused_windows(d->service.directory,offer.id)!=1) reason="Allocation identity was already used; request a new plan.";
    for (unsigned i=0;i<4;i++) {
        LmbWindowsDonorSlot *s=&d->slots[i];
        if ((s->transaction.reservation_held || s->transaction.phase==LMB_HOME_PENDING) &&
            !memcmp(s->transaction.offer.id,offer.id,32)) reason="BUSY: allocation already exists.";
        if (!target && !s->transaction.reservation_held && s->transaction.phase!=LMB_HOME_PENDING && s->client==LMB_INVALID_SOCKET && !s->release_pending) target=s;
    }
    if (!target && !reason) reason="BUSY: resident allocation capacity is full.";
    if (reason) {
        LmbWindowsDonorSlot rejected={.transaction={.offer=offer,.phase=LMB_HOME_REJECTED}};
        snprintf(rejected.transaction.reason,sizeof rejected.transaction.reason,"%s",reason);
        (void)lmb_donor_status_windows(fd,&rejected);goto done;
    }
    if (lmb_home_offer_begin(&target->transaction,&offer,d->tracker,lmb_donor_room_windows(d),
            d->machine.disk_available_bytes,GetTickCount64())) goto done;
    d->selected=(unsigned)(target-d->slots);d->service.snapshot.revision++;
    target->client=fd;fd=LMB_INVALID_SOCKET;
    if (lmb_donor_status_windows(target->client,target)) lmb_donor_lost_windows(d,target);
 done:
    lmb_msg_free(&m);if (fd!=LMB_INVALID_SOCKET) lmb_close(fd);
}
static void lmb_donor_message_windows(LmbWindowsDonor *d,LmbWindowsDonorSlot *s) {
    LmbMsg m={0};LmbHomeTransaction *t=&s->transaction;
    int bad=lmb_recv_bounded(s->client,&m,16384,1000) || m.body_len!=32 || m.pay_len || memcmp(m.body,t->offer.id,32);
    if (!bad) {
        t->last_seen_ms=GetTickCount64();
        switch (m.op) {
        case LMB_HOME_PULSE: break;
        case LMB_HOME_COMMIT:
            bad=lmb_home_commit(t,m.body);
            if (!bad && lmb_donor_launch_windows(d,s))
                (void)lmb_donor_release_windows(d,s,LMB_HOME_FAILED,"Cannot start the approved resident worker; see engines.log.");
            break;
        case LMB_HOME_CANCEL:
            (void)lmb_donor_release_windows(d,s,LMB_HOME_CLOSED,"Requester cancelled the plan.");break;
        case LMB_HOME_DETACH:
            if (!s->retained) bad=1;
            else { (void)lmb_donor_status_windows(s->client,s);lmb_donor_lost_windows(d,s); }
            break;
        default: bad=1;
        }
    }
    lmb_msg_free(&m);
    if (bad || (s->client!=LMB_INVALID_SOCKET && lmb_donor_status_windows(s->client,s))) lmb_donor_lost_windows(d,s);
}
static void lmb_donor_operation_windows(LmbWindowsDonor *d,int op) {
    LmbWindowsDonorSlot *s=&d->slots[d->selected];
    if (op==HOME_SVC_STOP) d->stopping=1;
    else if (op==HOME_SVC_ALLOW_REPEAT || op==HOME_SVC_REVOKE_REPEAT)
        home_service_disconnect(&d->service); /* native bounded policies are not available yet */
    else if (op==HOME_SVC_NEXT_MODEL) {
        for (unsigned i=1;i<=4;i++) {
            unsigned next=(d->selected+i)%4;
            if (d->slots[next].transaction.reservation_held || d->slots[next].transaction.phase==LMB_HOME_PENDING) {
                d->selected=next;break;
            }
        }
        d->service.snapshot.revision++;
    }
    else if (op==HOME_SVC_UNLOAD || op==HOME_SVC_CANCEL)
        (void)lmb_donor_release_windows(d,s,LMB_HOME_CLOSED,"Stopped by this computer's owner.");
    else if ((op==HOME_SVC_ACCEPT || op==HOME_SVC_DECLINE) && s->transaction.phase==LMB_HOME_PENDING) {
        int yes=op==HOME_SVC_ACCEPT;uint64_t room=lmb_donor_room_windows(d);
        /* Persist the owner decision before starting or acknowledging work. */
        snprintf(d->service.snapshot.detail,sizeof d->service.snapshot.detail,"Owner %s this immutable allocation.",yes ? "approved" : "declined");
        if (home_service_save(&d->service)) {d->stopping=1;return;}
        if (lmb_home_decide(&s->transaction,yes,room,d->machine.disk_available_bytes,GetTickCount64()))
            (void)lmb_donor_release_windows(d,s,LMB_HOME_FAILED,"Resources are no longer available.");
        if (s->client!=LMB_INVALID_SOCKET && lmb_donor_status_windows(s->client,s)) lmb_donor_lost_windows(d,s);
        d->service.snapshot.revision++;
    }
}
static int lmb_donor_step_windows(LmbWindowsDonor *d) {
    for (unsigned i=0;i<4;i++) {
        LmbWindowsDonorSlot *s=&d->slots[i];
        if (s->release_pending) {(void)lmb_donor_release_windows(d,s,s->release_phase,s->release_reason);continue;}
        if (s->worker.process && WaitForSingleObject(s->worker.process,0)!=WAIT_TIMEOUT) {
            (void)lmb_donor_release_windows(d,s,LMB_HOME_FAILED,"The owned Segment engine stopped.");continue;
        }
        if (s->transaction.phase==LMB_HOME_LOADING && s->ready && s->ready!=INVALID_HANDLE_VALUE &&
            WaitForSingleObject(s->ready,0)==WAIT_OBJECT_0) {
            char status=0;
            if (lmb_pipe_io(s->preflight,&status,1,0,GetTickCount64()+200) || status!='P' ||
                lmb_home_mark_segment_ready(&s->transaction)) {
                (void)lmb_donor_release_windows(d,s,LMB_HOME_FAILED,"Worker did not confirm resident preparation.");continue;
            }
            s->retained=1;lmb_donor_handle_close(&s->preflight);lmb_donor_handle_close(&s->handoff);lmb_donor_handle_close(&s->ready);
            lmb_socket_close(s->listener);s->listener=LMB_INVALID_SOCKET;
            if (s->client!=LMB_INVALID_SOCKET && lmb_donor_status_windows(s->client,s)) lmb_donor_lost_windows(d,s);
        }
        if (s->client!=LMB_INVALID_SOCKET && lmb_socket_wait(s->client,0,0)==1) lmb_donor_message_windows(d,s);
        if (!s->retained && lmb_home_expired(&s->transaction,GetTickCount64()))
            (void)lmb_donor_release_windows(d,s,LMB_HOME_CLOSED,"Request lease expired before preparation completed.");
    }
    lmb_donor_snapshot_windows(d);
    int operation=home_service_poll(&d->service);
    if (operation>0) lmb_donor_operation_windows(d,operation);
    lmb_donor_snapshot_windows(d);
    if (home_service_save(&d->service)) d->stopping=1;
    home_service_answer(&d->service);
    if (d->stopping || atomic_load(&d->compute.stop)) return 1;
    if (d->inventory_started && GetTickCount64()>=d->next_inventory) {
        LmbMachineReport report;lmb_donor_inventory_windows(d,&report);
        EnterCriticalSection(&d->inventory.lock);
        memcpy(report.identity,d->inventory.report.identity,32);d->inventory.report=report;
        LeaveCriticalSection(&d->inventory.lock);d->next_inventory=GetTickCount64()+500;
    }
    if (lmb_socket_wait(d->listener,0,0)==1) {
        LmbSocket incoming=lmb_socket_accept(d->listener,NULL,NULL);
        if (incoming!=LMB_INVALID_SOCKET) lmb_donor_incoming_windows(d,incoming);
    }
    return 0;
}
static void lmb_donor_close_windows(LmbWindowsDonor *d) {
    if (d->inventory_started) {
        lmb_inventory_stop_windows(&d->inventory);DeleteCriticalSection(&d->inventory.lock);d->inventory_started=0;
    }
    if (d->listener!=LMB_INVALID_SOCKET) lmb_socket_close(d->listener);
    d->listener=LMB_INVALID_SOCKET;
    for (unsigned i=0;i<4;i++) {
        LmbWindowsDonorSlot *s=&d->slots[i];
        if (s->transaction.phase || s->worker.process) {
            uint64_t until=GetTickCount64()+5000;
            while (lmb_donor_release_windows(d,s,LMB_HOME_CLOSED,"This computer stopped sharing.") && GetTickCount64()<until) Sleep(10);
        }
    }
    lmb_compute_broker_stop(&d->compute);
    if (d->opened) {
        lmb_donor_snapshot_windows(d);
        d->service.snapshot.state=d->service.snapshot.reserved_total ? HOME_SVC_FAILED : HOME_SVC_STOPPED;
        (void)home_service_save(&d->service);home_service_close(&d->service);d->opened=0;
    }
    if (d->lease>=0) {_close(d->lease);d->lease=-1;}
    lmb_donor_handle_close(&d->executable);
}
static int lmb_donor_open_windows(LmbWindowsDonor *d,const char *worker,const char *tracker,uint64_t limit) {
    memset(d,0,sizeof *d);d->listener=LMB_INVALID_SOCKET;d->lease=-1;d->executable=INVALID_HANDLE_VALUE;
    for (unsigned i=0;i<4;i++) {
        d->slots[i].client=d->slots[i].listener=LMB_INVALID_SOCKET;
        d->slots[i].control=d->slots[i].handoff=d->slots[i].lifetime=d->slots[i].preflight=d->slots[i].ready=INVALID_HANDLE_VALUE;
    }
    if (!limit || strlen(worker)>=sizeof d->worker || strlen(tracker)>=sizeof d->tracker ||
        !*tracker || lmb_inventory_text(tracker) || !lmb_secure_enabled()) return -1;
    const char *token=getenv("LUMABRI_TOKEN");
    if (!token || !*token || strlen(token)>LMB_TOKEN_MAX) return -1;
    LmbSocket check=lmb_connect_ms_io(tracker,1500,1000);LmbMsg response={0};
    int bad=check==LMB_INVALID_SOCKET || lmb_auth(check) ||
        lmb_send(check,LMB_CHALLENGE,NULL,0,NULL,0) || lmb_recv_bounded(check,&response,64,1000) ||
        response.op!=LMB_CHALLENGE_R || response.body_len!=32 || response.pay_len;
    lmb_msg_free(&response);if (check!=LMB_INVALID_SOCKET) lmb_close(check);
    if (bad) return -1;
    snprintf(d->worker,sizeof d->worker,"%s",worker);snprintf(d->tracker,sizeof d->tracker,"%s",tracker);
    if (home_service_open(&d->service,"donor")) return -1;
    d->opened=1;
    /* Deny replacement while this keeper advertises and launches the binary. */
    wchar_t *wide=lmb_process_wide(worker);
    if (!wide) goto bad;
    d->executable=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);free(wide);
    BY_HANDLE_FILE_INFORMATION info;
    if (d->executable==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(d->executable,&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) ||
        lmb_runtime_thread_capacity(worker,&d->thread_capacity) ||
        lmb_machine_probe(&d->machine,d->service.directory,NULL)) goto bad;
    d->reserve=lmb_machine_ram_reserve();
    d->budget=d->machine.ram_available_bytes>d->reserve ? d->machine.ram_available_bytes-d->reserve : 0;
    if (d->budget>limit) d->budget=limit;
    if (!d->budget || lmb_home_advertise_ip(d->ip,sizeof d->ip)) goto bad;
    d->lease=lmb_machine_compute_lease_acquire("household",tracker,NULL,0);
    if (d->lease<0 || snprintf(d->compute_path,sizeof d->compute_path,"%s/compute",d->service.directory)>=(int)sizeof d->compute_path ||
        lmb_compute_broker_start(&d->compute,d->compute_path)) goto bad;
    int port=0;d->listener=lmb_home_listen_service(&port);
    if (d->listener==LMB_INVALID_SOCKET || snprintf(d->control,sizeof d->control,"%s:%d",d->ip,port)>=(int)sizeof d->control) goto bad;
    snprintf(d->service.snapshot.name,sizeof d->service.snapshot.name,"%s",d->machine.hostname);
    d->service.snapshot.revision=1;lmb_donor_snapshot_windows(d);
    if (home_service_save(&d->service) || lmb_donor_runtime_id_windows(d)) goto bad;
    LmbMachineReport report;lmb_donor_inventory_windows(d,&report);
    if (lmb_inventory_start_windows(&d->inventory,tracker,&report)) goto bad;
    d->inventory_started=1;d->next_inventory=GetTickCount64()+500;
    return 0;
 bad:
    lmb_donor_close_windows(d);return -1;
}
#endif
