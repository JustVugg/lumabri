/* Read-only operator snapshot. Uses the same leased reports, saved plans and
 * calibration keys as the TUI/planner. No model open, preparation or eviction.
 * A saved plan and a reporting machine do not prove that a host is healthy. */
#ifndef LMB_WORKSPACE_H
#define LMB_WORKSPACE_H
#include "lumabri_operator_access.h"
typedef struct {
    LmbMachineReport nodes[LMB_INVENTORY_MAX]; uint32_t node_count;
    LmbResidentPlan plans[64]; size_t plan_count;
    LmbModelRoute routes[LMB_ROUTE_MAX]; size_t route_count;
    int inventory_ok, registry_ok;
    uint64_t captured_at;
} LmbWorkspace;
static int workspace_capture(int access, const char *tracker, LmbWorkspace *s) {
    memset(s,0,sizeof *s); s->captured_at=(uint64_t)time(NULL);
    s->inventory_ok=!lmb_inventory_fetch(tracker,s->nodes,&s->node_count);
    if (!s->inventory_ok) s->node_count=0;
    s->plan_count=home_resident_library_list(tracker,s->plans,64);
    int dir=lmb_route_dir(access,0);
    if (dir<0) { s->registry_ok=errno==ENOENT; return 0; }
    LmbRouteRegistry *snapshot=calloc(1,sizeof *snapshot);
    s->registry_ok=snapshot && !lmb_route_registry_read(dir,snapshot);
    for (uint32_t i=0;s->registry_ok && i<snapshot->count;i++) {
        const LmbModelRoute *route=&snapshot->routes[i];
        if (!strcmp(route->tracker,tracker)) s->routes[s->route_count++]=*route;
    }
    if (!s->registry_ok) s->route_count=0;
    free(snapshot); close(dir); return 0;
}
static const LmbMachineReport *workspace_node(const LmbWorkspace *s, const uint8_t id[32]) {
    for (uint32_t i=0;i<s->node_count;i++) if (!memcmp(id,s->nodes[i].identity,32)) return &s->nodes[i];
    return NULL;
}
static int workspace_node_json(Cap *body, const LmbMachineReport *r) {
    char id[65]; lmb_hex(id,r->identity,32);
    const LmbMachineProfile *m=&r->machine; const LmbResourceFacts *f=&r->facts;
    int bad=api_addf(body,"{\"id\":\"%s\",\"name\":",id) || api_json_text(body,m->hostname) ||
        cap_str(body,",\"cpu\":") || api_json_text(body,m->cpu_model) ||
        cap_str(body,",\"os\":") || api_json_text(body,m->os) ||
        cap_str(body,",\"arch\":") || api_json_text(body,m->arch) ||
        api_addf(body,",\"age_ms\":%u,\"threads\":%u,\"runtime_threads\":%u,\"ram_total_bytes\":%llu,"
            "\"ram_available_bytes\":%llu,\"ram_offered_bytes\":%llu,\"gpu_detected\":%u,\"vram_inventory_bytes\":%llu,"
            "\"gpu_execution_verified\":false,\"load_one\":",r->age_ms,m->logical_cpus,r->runtime_threads,
            (unsigned long long)m->ram_total_bytes,(unsigned long long)m->ram_available_bytes,
            (unsigned long long)r->ram_budget_bytes,m->gpu_count,(unsigned long long)m->vram_total_bytes);
    if (!bad) bad=f->known&LMB_FACT_LOAD ? api_addf(body,"%.3f",f->load_milli/1000.0) : cap_str(body,"null");
    if (!bad) bad=cap_str(body,",\"workload\":");
    const LmbWorkloadFacts *w=&r->workload;
    if (!bad) bad=w->known ? api_addf(body,"{\"allocations\":%u,\"reserved_bytes\":%llu,\"active\":%u,\"queued\":%u}",
        w->allocations,(unsigned long long)w->reserved_bytes,w->active,w->queued) : cap_str(body,"null");
    if (!bad) bad=cap_str(body,",\"machine_cost\":");
    if (!bad) bad=f->known&LMB_FACT_PRICE ? api_addf(body,
        "{\"state\":\"declared\",\"currency\":\"%.3s\",\"micro_units_per_hour\":%llu}",
        f->currency,(unsigned long long)f->price_micro_per_hour) : cap_str(body,"null");
    if (!bad) bad=cap_str(body,",\"power\":");
    if (!bad) bad=f->known&LMB_FACT_POWER ? api_addf(body,"{\"state\":\"declared_estimate\",\"watts\":%.3f}",
        f->power_milliwatts/1000.0) : cap_str(body,"null");
    return bad || cap_str(body,",\"energy_joules\":null}") ? -1 : 0;
}
/* Compare a saved observation to this single inventory snapshot. Never
 * repaint a previous configuration's rate as current after a plan changes. */
static int workspace_observation(Cap *body, const LmbWorkspace *s, const LmbResidentPlan *p) {
    char records[1200], why[200]; LmbCalibration prior, seed;
    if (catalog_calibration_dir(records) || lmb_cal_load(records,p->content_id,&prior))
        return cap_str(body,"{\"state\":\"unknown\",\"decode_tok_s\":null}");
    int current=s->inventory_ok && !home_resident_observation_seed(p,&seed,records) &&
        !catalog_runtime_match(&seed.key,s->nodes,s->node_count,why,sizeof why);
    for (uint32_t i=0;current && i<seed.key.nodes;i++) {
        const LmbMachineReport *node=workspace_node(s,p->peer_keys[i]);
        if (!node || !node->workload.known || !node->workload.allocations ||
            node->workload.reserved_bytes<p->execution.nodes[i].reserved_bytes) { current=0; break; }
        seed.key.workload[i]=node->workload; seed.key.workload[i].active=seed.key.workload[i].queued=0;
    }
    if (current) {
        seed.key.adapter_abi=prior.key.adapter_abi;
        snprintf(seed.key.numeric_class,sizeof seed.key.numeric_class,"%s",prior.key.numeric_class);
        LmbCalibration matching;
        if (!lmb_cal_profile_load(records,&seed.key,&matching)) prior=matching;
        current=lmb_cal_matches(&seed.key,&prior.key);
    }
    if (!current) return cap_str(body,"{\"state\":\"obsolete\",\"decode_tok_s\":null}");
    return api_addf(body,"{\"state\":\"measured\",\"decode_tok_s\":%.6f,\"ttft_seconds\":%.6f,"
        "\"observed_at\":%.0f,\"samples\":%u,\"prompt_tokens\":%u,\"generated_tokens\":%u,"
        "\"scope\":\"last_matching_turn_not_capacity_guarantee\"}",
        prior.decode_tok_s,prior.ttft_seconds,prior.measured_at,prior.samples,prior.prompt_tokens,prior.generated_tokens);
}
static int workspace_json(Cap *body, const LmbWorkspace *s) {
    int bad=api_addf(body,"{\"schema\":1,\"captured_at\":%llu,\"inventory_ok\":%s,\"registry_ok\":%s,"
        "\"inventory_ttl_ms\":%u,\"nodes\":[",(unsigned long long)s->captured_at,s->inventory_ok ? "true" : "false",
        s->registry_ok ? "true" : "false",LMB_INVENTORY_TTL_MS);
    for (uint32_t i=0;i<s->node_count && !bad;i++)
        bad=(i && cap_str(body,",")) || workspace_node_json(body,&s->nodes[i]);
    if (!bad) bad=cap_str(body,"],\"allocations\":[");
    for (size_t i=0;i<s->plan_count && !bad;i++) {
        const LmbResidentPlan *p=&s->plans[i]; char id[65]; lmb_hex(id,p->allocation,32);
        bad=api_addf(body,"%s{\"id\":\"%s\",\"name\":",i ? "," : "",id) || api_json_text(body,p->model) ||
            cap_str(body,",\"adapter\":") || api_json_text(body,p->observation.adapter) ||
            api_addf(body,",\"state\":\"saved_plan\",\"context\":%u,\"session_limit\":%u,"
                "\"preparation_seconds\":",p->context,p->sessions ? p->sessions : 1);
        if (!bad) bad=p->preparation_seconds>0 ? api_addf(body,"%.6f",p->preparation_seconds) : cap_str(body,"null");
        if (!bad) bad=cap_str(body,",\"ranges\":[");
        for (uint32_t k=0;k<p->execution.count && !bad;k++) {
            lmb_hex(id,p->peer_keys[k],32);
            bad=api_addf(body,"%s{\"node\":\"%s\",\"begin\":%u,\"end\":%u,\"edge\":%s,"
                "\"reserved_bytes\":%llu,\"report_present\":%s}",k ? "," : "",id,
                p->execution.nodes[k].begin,p->execution.nodes[k].end,p->execution.nodes[k].edge ? "true" : "false",
                (unsigned long long)p->execution.nodes[k].reserved_bytes,workspace_node(s,p->peer_keys[k]) ? "true" : "false");
        }
        if (!bad) bad=cap_str(body,"],\"observation\":") || workspace_observation(body,s,p) || cap_str(body,"}");
    }
    if (!bad) bad=cap_str(body,"],\"models\":[");
    for (size_t i=0;i<s->route_count && !bad;i++) bad=api_route_record(body,&s->routes[i],i!=0,1);
    return bad || cap_str(body,"]}\n") ? -1 : 0;
}
static int api_workspace(int fd, int access, const char *tracker) {
    LmbWorkspace *snapshot=calloc(1,sizeof *snapshot); Cap body={0};
    int bad=!snapshot || workspace_capture(access,tracker,snapshot) || workspace_json(&body,snapshot);
    if (!bad) bad=fd<0 ? fputs(body.p,stdout)==EOF : api_response(fd,200,body.p);
    else if (fd>=0) api_error(fd,503,"workspace_unavailable");
    free(snapshot); free(body.p); return bad ? -1 : 0;
}
#endif
