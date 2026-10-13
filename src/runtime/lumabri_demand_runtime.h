/* Internal parent/child telemetry, never a network protocol. Snapshot reads
 * are fork-consistent; the observation age is reported, not hidden. */
#ifndef LMB_DEMAND_RUNTIME_H
#define LMB_DEMAND_RUNTIME_H
#include "lumabri_demand.h"
static LmbDemand api_demand;
static int api_demand_fd=-1,api_demand_failed;
static LmbDemandEvent api_demand_tx;
static void api_demand_send(unsigned kind,unsigned outcome,uint64_t ttft,uint64_t total) {
    if (api_demand_fd<0) return;
    api_demand_tx.sequence++; api_demand_tx.kind=kind; api_demand_tx.outcome=outcome;
    api_demand_tx.ttft_us=ttft; api_demand_tx.completion_us=total;
    ssize_t n;
    do n=send(api_demand_fd,&api_demand_tx,sizeof api_demand_tx,MSG_DONTWAIT);
    while (n<0 && errno==EINTR);
    if (n!=(ssize_t)sizeof api_demand_tx) api_demand_failed=1;
}
static void api_demand_drain(int fd,LmbDemandWorker *w) {
    for (;;) {
        unsigned char bytes[sizeof(LmbDemandEvent)+1];
        ssize_t n=recv(fd,bytes,sizeof bytes,MSG_DONTWAIT);
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) break;
        /* A connected datagram peer can close with EOF or a disconnect
         * error; Linux normally keeps returning EAGAIN. Reaping, not this
         * platform-specific indication, verifies END and child exit status. */
        if (!n || (n<0 && (errno==ECONNRESET || errno==ENOTCONN))) break;
        if (n<0) {
            if (!w->uncertain) fprintf(stderr,"[demand] channel read failed: errno=%d\n",errno);
            lmb_demand_worker_loss(&api_demand,w,lmb_io_monotonic_ms()); break;
        }
        if (n!=(ssize_t)sizeof(LmbDemandEvent)) { lmb_demand_worker_loss(&api_demand,w,lmb_io_monotonic_ms()); break; }
        LmbDemandEvent e; memcpy(&e,bytes,sizeof e);
        (void)lmb_demand_event(&api_demand,w,&e,lmb_io_monotonic_ms());
    }
}
static int api_demand_json(Cap *body) {
    if (!api_demand.started_ms) return cap_str(body,"null");
    uint64_t now=lmb_io_monotonic_ms(),age=now>=api_demand.captured_ms ? now-api_demand.captured_ms : UINT64_MAX;
    int full=lmb_demand_window_complete(&api_demand,now);
    char instance[65]; lmb_hex(instance,api_demand.instance,32);
    int bad=api_addf(body,"{\"schema\":1,\"instance\":\"%s\",\"scope\":\"this_gateway_authorized_chat_only\","
        "\"window_seconds\":60,\"bucket_seconds\":1,\"snapshot_age_ms\":%llu,\"window_complete\":%s,"
        "\"telemetry_gaps\":%llu,\"uncertain_requests\":%u,\"connection_limit_rejections\":%llu,\"models\":[",instance,
        (unsigned long long)age,full ? "true" : "false",
        (unsigned long long)api_demand.loss_count,api_demand.uncertain_workers,(unsigned long long)api_demand.refused_connections);
    unsigned count=0;
    for (unsigned i=0;i<LMB_DEMAND_MODELS && !bad;i++) {
        const LmbDemandModel *m=&api_demand.models[i];
        if (!m->last_activity || (!m->pending && !m->admitted && now>=m->last_activity &&
            now-m->last_activity>=(LMB_DEMAND_WINDOW+1)*1000)) continue;
        char id[65]; lmb_hex(id,m->id,32); LmbDemandSummary s; lmb_demand_summary(m,now,&s);
        bad=api_addf(body,"%s{\"id\":\"%s\",\"opening\":%u,\"admitted\":%u,\"started\":%llu,"
            "\"accepted\":%llu,\"completed\":%llu,\"capacity_rejected\":%llu,\"failed\":%llu,"
            "\"cancelled\":%llu,\"invalid\":%llu,\"interrupted\":%llu",
            count++ ? "," : "",id,m->pending,m->admitted,
            (unsigned long long)s.counts[LMB_DEMAND_STARTED],(unsigned long long)s.counts[LMB_DEMAND_ADMITTED],
            (unsigned long long)s.counts[LMB_DEMAND_COMPLETE],(unsigned long long)s.counts[LMB_DEMAND_CAPACITY],
            (unsigned long long)s.counts[LMB_DEMAND_FAILED],(unsigned long long)s.counts[LMB_DEMAND_CANCELLED],
            (unsigned long long)s.counts[LMB_DEMAND_INVALID],(unsigned long long)s.counts[LMB_DEMAND_INTERRUPTED]);
        if (!bad) bad=api_addf(body,",\"latency\":{\"scope\":\"last_64_successful_turns_in_window\","
            "\"ttft_samples\":%u,\"completion_samples\":%u,\"ttft_ms\":",s.ttft_samples,s.completion_samples);
        if (!bad) bad=s.ttft_samples ? api_addf(body,"{\"p50\":%.3f,\"p95\":%.3f}",s.ttft_p50_us/1000.0,s.ttft_p95_us/1000.0) : cap_str(body,"null");
        if (!bad) bad=cap_str(body,",\"completion_ms\":");
        if (!bad) bad=s.completion_samples ? api_addf(body,"{\"p50\":%.3f,\"p95\":%.3f}",s.completion_p50_us/1000.0,s.completion_p95_us/1000.0) : cap_str(body,"null");
        if (!bad) bad=cap_str(body,"}}");
    }
    return bad || cap_str(body,"]}") ? -1 : 0;
}
static void api_demand_http(int fd) {
    Cap body={0};
    if (api_demand_json(&body) || !api_demand.started_ms) api_error(fd,503,"demand_observer_unavailable");
    else (void)api_response(fd,200,body.p);
    free(body.p);
}
#endif
