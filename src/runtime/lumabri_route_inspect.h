/* Read-only explanation of replica preference. The caller checks operator
 * permission; no chat permit, control mutation or engine connection is made. */
#ifndef LMB_ROUTE_INSPECT_H
#define LMB_ROUTE_INSPECT_H
static int api_route_decision_json(Cap *body,const LmbModelRoute *route,const LmbRouteDecision *d) {
    char id[65];lmb_hex(id,route->id,32);
    int bad=api_addf(body,"{\"schema\":1,\"model\":\"%s\",\"revision\":%u,\"captured_at\":%.0f,"
        "\"policy\":\"%s\",\"reason\":\"%s\",\"inventory_ok\":%s,"
        "\"scope\":\"preference_snapshot_not_admission\",\"replicas\":[",id,route->revision,d->captured_at,
        lmb_route_policy_name(route->policy),d->reason,d->inventory_ok ? "true" : "false");
    for (uint32_t rank=0;rank<route->count && !bad;rank++) {
        uint32_t index=d->order[rank];if (index>=route->count) return -1;
        const LmbReplicaEvidence *e=&d->facts[index];lmb_hex(id,route->replicas[index].allocation,32);
        int usable=d->inventory_ok && e->usable;
        bad=api_addf(body,"%s{\"allocation\":\"%s\",\"rank\":%u,\"operator_position\":%u,\"plan_matches\":%s,"
            "\"load\":",rank ? "," : "",id,rank+1,index+1,d->inventory_ok ? e->usable ? "true" : "false" : "null");
        int load=usable && lmb_replica_load_valid(&e->load);
        if (!bad) bad=load ? api_addf(body,"{\"nodes\":%u,\"max_pending_turns\":%u,\"total_pending_turns\":%u,"
            "\"oldest_report_ms\":%u}",e->load.nodes,e->load.max_pending,e->load.total_pending,e->load.max_age_ms) : cap_str(body,"null");
        int timing=usable && !lmb_replica_timing_evidence(e,1,d->captured_at,1);
        int decode=usable && !lmb_replica_timing_evidence(e,1,d->captured_at,0);
        if (!bad) bad=cap_str(body,",\"timing\":");
        if (!bad && (timing || decode)) {
            bad=api_addf(body,"{\"observed_at\":%.0f,\"source\":\"%s\",\"prompt_tokens\":%u,\"generated_tokens\":%u,"
                "\"observed_ttft_seconds\":",e->measured_at,lmb_cal_source_name(e->observation_source),e->prompt_tokens,e->generated_tokens);
            if (!bad) bad=timing ? api_addf(body,"%.6f",e->ttft_seconds) : cap_str(body,"null");
            if (!bad) bad=cap_str(body,",\"decode_tok_s\":");
            if (!bad) bad=decode ? api_addf(body,"%.6f",e->decode_tok_s) : cap_str(body,"null");
            if (!bad) bad=cap_str(body,"}");
        } else if (!bad) bad=cap_str(body,"null");
        if (!bad) bad=cap_str(body,",\"declared_machine_cost\":");
        if (!bad) bad=usable && e->price_known ? api_addf(body,"{\"currency\":\"%.3s\",\"micro_units_per_hour\":\"%llu\"}",
            e->currency,(unsigned long long)e->micro_per_hour) : cap_str(body,"null");
        if (!bad) bad=cap_str(body,"}");
    }
    return bad || cap_str(body,"]}\n") ? -1 : 0;
}
static int api_route_inspect(int fd,int access,const char *tracker,const char *text) {
    uint8_t id[32];LmbModelRoute route;LmbRouteDecision decision;Cap body={0};
    unsigned status=200;const char *error="routing_observation_unavailable";
    if (!text || strlen(text)!=64 || lmb_unhex(id,text,32)) {status=400;error="invalid_model_id";}
    else {
        int rc=api_route_read(access,id,tracker,&route);
        if (rc==LMB_ROUTE_MISSING) {status=404;error="managed_model_not_found";}
        else if (rc || api_route_capture(tracker,&route,&decision) || api_route_decision_json(&body,&route,&decision)) status=503;
    }
    if (status!=200) {
        if (fd>=0) api_error(fd,status,error);else fprintf(stderr,"%s\n",error);
    } else if (fd>=0) (void)api_response(fd,status,body.p);
    else if (fputs(body.p,stdout)==EOF) status=500;
    free(body.p);return status==200 ? 0 : 1;
}
#endif
