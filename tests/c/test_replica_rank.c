#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/planner/lumabri_replica_rank.h"
#include "src/planner/lumabri_calibration_profiles.h"
#include <assert.h>
#include <sys/wait.h>

static LmbCalibration observation(unsigned replica, double at) {
    LmbCalibration c={0}; LmbCalKey *k=&c.key;
    memset(k->model_root,'a',64); strcpy(k->adapter,"olmoe"); k->adapter_abi=1;
    strcpy(k->numeric_class,"cpu-f32"); strcpy(k->build_id,"runtime"); strcpy(k->plan_kind,"segment");
    k->nodes=1; k->context=128; k->sessions=1;
    snprintf(k->node_id[0],sizeof k->node_id[0],"replica-%u",replica);
    strcpy(k->node_hardware_id[0],"hardware"); strcpy(k->node_build_id[0],"engine"); strcpy(k->node_backend[0],"cpu");
    k->layer_end[0]=2; k->threads[0]=4;
    c.decode_tok_s=5+replica; c.ttft_seconds=.2; c.measured_at=at;
    c.prompt_tokens=4; c.generated_tokens=8; c.samples=1; c.source=LMB_CAL_SOURCE_SESSION;
    assert(lmb_cal_valid(&c)); return c;
}
static void profile_tests(void) {
    char path[]="/tmp/lmb-replica-profiles-XXXXXX"; assert(mkdtemp(path));
    LmbCalibration a=observation(0,1000),b=observation(1,1001),got;
    assert(!lmb_cal_profile_store(path,&a)); assert(!lmb_cal_profile_store(path,&b));
    assert(!lmb_cal_profile_load(path,&a.key,&got) && got.decode_tok_s==5);
    assert(!lmb_cal_profile_load(path,&b.key,&got) && got.decode_tok_s==6);
    LmbCalibration changed=a; changed.key.threads[0]++;
    assert(lmb_cal_profile_load(path,&changed.key,&got) && !got.samples);
    changed=a; changed.key.workload[0]=(LmbWorkloadFacts){.known=1,.allocations=1,
        .compute_policy=LMB_COMPUTE_LOCAL_FIFO,.reserved_bytes=1024,.allocation_set={1}};
    assert(lmb_cal_profile_load(path,&changed.key,&got));
    a.measured_at=1002; a.decode_tok_s=7; assert(!lmb_cal_profile_store(path,&a));
    changed=a; changed.measured_at=999; changed.decode_tok_s=99;
    assert(!lmb_cal_profile_store(path,&changed));
    assert(!lmb_cal_profile_load(path,&a.key,&got) && got.decode_tok_s==7);
    int dir=lmb_cal_profile_dir(path,a.key.model_root,0); assert(dir>=0);
    assert(!fchmodat(dir,"0.cal",0644,0)); assert(lmb_cal_profile_load(path,&a.key,&got));
    assert(!fchmodat(dir,"0.cal",0600,0));
    assert(!symlinkat("0.cal",dir,"pending")); assert(lmb_cal_profile_store(path,&a));
    assert(!unlinkat(dir,"pending",0));
    /* A separate process holding the writer lock must not stall generation. */
    int lock=openat(dir,"write.lock",O_RDWR); assert(lock>=0 && !flock(lock,LOCK_EX|LOCK_NB));
    pid_t pid=fork(); assert(pid>=0);
    if (!pid) { close(lock); _exit(lmb_cal_profile_store(path,&a) ? 0 : 1); }
    int status; assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status)); close(lock);
    for (unsigned i=2;i<LMB_CAL_PROFILE_SLOTS+1;i++) {
        changed=observation(i,1002+i); assert(!lmb_cal_profile_store(path,&changed));
    }
    assert(lmb_cal_profile_load(path,&b.key,&got)); /* oldest, not most recently updated a */
    assert(!lmb_cal_profile_load(path,&a.key,&got));
    assert(!lmb_cal_profile_load(path,&changed.key,&got));
    assert(!unlinkat(dir,"1.cal",0)); assert(!symlinkat("0.cal",dir,"1.cal"));
    assert(lmb_cal_profile_load(path,&changed.key,&got)); /* symlink is not evidence */
    for (unsigned i=0;i<LMB_CAL_PROFILE_SLOTS;i++) { char name[24]; snprintf(name,sizeof name,"%u.cal",i); assert(!unlinkat(dir,name,0)); }
    assert(!unlinkat(dir,"write.lock",0)); close(dir);
    char sub[512]; snprintf(sub,sizeof sub,"%s/%s.profiles",path,a.key.model_root); assert(!rmdir(sub)); assert(!rmdir(path));
}
static void rank_tests(void) {
    LmbReplicaEvidence e[3]={0}; uint32_t order[LMB_ROUTE_REPLICAS];
    for (unsigned i=0;i<3;i++) e[i]=(LmbReplicaEvidence){.usable=1,.observation_current=1,.price_known=1,
        .decode_tok_s=5+i,.measured_at=1000,.prompt_tokens=4,.generated_tokens=8,.micro_per_hour=300-i*100,.currency="EUR"};
    assert(!strcmp(lmb_replica_rank(0,e,3,1000,order),"operator_order") && order[0]==0);
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"recent_matching_decode_observations") && order[0]==2);
    assert(!strcmp(lmb_replica_rank(2,e,3,1000,order),"declared_machine_footprint") && order[0]==2);
    e[2].price_known=0;
    assert(!strcmp(lmb_replica_rank(2,e,3,1000,order),"prices_missing_or_mixed_currency") && order[0]==0);
    e[2].price_known=1; strcpy(e[2].currency,"USD");
    assert(!strcmp(lmb_replica_rank(2,e,3,1000,order),"prices_missing_or_mixed_currency") && order[0]==0);
    e[0].observation_current=0;
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"observations_missing_or_expired") && order[0]==0);
    e[0].observation_current=1; e[1].prompt_tokens++;
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"observation_lengths_differ") && order[0]==0);
    e[1].prompt_tokens--; e[2].generated_tokens=7;
    assert(strcmp(lmb_replica_rank(1,e,3,1000,order),"recent_matching_decode_observations"));
    e[2].generated_tokens=8;
    assert(!strcmp(lmb_replica_rank(1,e,3,1301,order),"observations_missing_or_expired"));
    assert(!strcmp(lmb_replica_rank(1,e,3,999,order),"observations_missing_or_expired"));
    e[0].decode_tok_s=NAN;
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"observations_missing_or_expired"));
    e[0].decode_tok_s=6.99;
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"recent_matching_decode_observations") && order[0]==0);
    e[0].decode_tok_s=5; e[1].decode_tok_s=5.2; e[2].decode_tok_s=5.4;
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"recent_matching_decode_observations") && order[0]==1);
    e[0].decode_tok_s=6.99; e[1].decode_tok_s=6; e[2].decode_tok_s=7;
    e[0].usable=0;
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"recent_matching_decode_observations") && order[0]==2);
    e[0].usable=e[1].usable=e[2].usable=0;
    assert(!strcmp(lmb_replica_rank(1,e,3,1000,order),"no_approved_plan"));
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_POLICY_LAST+1,e,3,1000,order),"invalid_policy"));
}
static void load_tests(void) {
    LmbWorkloadFacts work={.known=1,.allocations=2,.compute_policy=LMB_COMPUTE_LOCAL_FIFO,
        .active=1,.queued=3,.reserved_bytes=4096,.allocation_set={1}};
    LmbReplicaLoad load={0};
    assert(!lmb_replica_load_add(&load,&work,8192,2048,1000));
    assert(load.nodes==1 && load.max_pending==4 && load.total_pending==4);
    work.queued=1;
    assert(!lmb_replica_load_add(&load,&work,8192,2048,9000));
    assert(load.nodes==2 && load.max_pending==4 && load.total_pending==6 && load.max_age_ms==9000);
    assert(lmb_replica_load_valid(&load));
    LmbReplicaLoad saved=load;
    assert(lmb_replica_load_add(&load,&work,8192,2048,LMB_REPLICA_LOAD_MAX_AGE_MS+1));
    assert(!memcmp(&load,&saved,sizeof load));
    assert(lmb_replica_load_add(&load,&work,8192,4097,1));
    assert(lmb_replica_load_add(&load,&work,2048,2048,1));
    work.known=0; assert(lmb_replica_load_add(&load,&work,8192,2048,1)); work.known=1;
    work.compute_policy=LMB_COMPUTE_UNKNOWN;
    assert(lmb_replica_load_add(&load,&work,8192,2048,1));
    work.compute_policy=LMB_COMPUTE_LOCAL_FIFO; work.queued=UINT32_MAX;
    assert(lmb_replica_load_add(&load,&work,8192,2048,1));

    LmbReplicaEvidence e[3]={0}; uint32_t order[LMB_ROUTE_REPLICAS];
    for (unsigned i=0;i<3;i++) e[i]=(LmbReplicaEvidence){.usable=1,.observation_current=1,
        .decode_tok_s=10-i*2,.measured_at=1000,.prompt_tokens=4,.generated_tokens=8,
        .load={.nodes=2,.max_pending=2,.total_pending=3,.max_age_ms=1000}};
    /* A fast replica contending with another model must not outrank an idle
     * approved chain. A shared machine is scored at its whole FIFO load. */
    e[2].load.max_pending=e[2].load.total_pending=0;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),
        "recent_workload_and_decode_observations") && order[0]==2 && order[1]==0);
    e[2].observation_current=0;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),
        "recent_workload_observations") && order[0]==2);
    /* Unknown load never wins by pretending zero; leave the explicit order. */
    e[1].load.nodes=0;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),
        "workload_missing_or_expired") && order[0]==0);
    e[1].load=e[0].load; e[1].load.max_age_ms=LMB_REPLICA_LOAD_MAX_AGE_MS+1;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),"workload_missing_or_expired"));
    e[1].load=e[0].load; e[1].load.total_pending=UINT32_MAX;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),"workload_missing_or_expired"));
    /* Equal peak pressure: prefer less work along the whole chain. */
    e[1].load=e[0].load; e[2].load=e[0].load; e[1].load.total_pending=2;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),
        "recent_workload_observations") && order[0]==1);
    /* Decode is only a comparable-observation tie breaker, with the same
     * noise band as observed-decode. It cannot bypass a lower-load tier. */
    e[2].observation_current=1; e[2].decode_tok_s=100;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),
        "recent_workload_and_decode_observations") && order[0]==1 && order[1]==2);
    e[0].load=e[1].load=e[2].load; e[0].decode_tok_s=96;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),
        "recent_workload_and_decode_observations") && order[0]==0);
    e[0].usable=0; e[0].load.nodes=0;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),
        "recent_workload_and_decode_observations") && order[0]==2 && order[2]==0);
    e[1].usable=e[2].usable=0;
    assert(!strcmp(lmb_replica_rank(LMB_ROUTE_LOAD_AWARE,e,3,1000,order),"no_approved_plan"));
}
static void latency_tests(void) {
    LmbReplicaEvidence e[3]={0}; uint32_t order[LMB_ROUTE_REPLICAS];
    const char *ranked="recent_workload_and_ttft_observations",*load_only="recent_workload_observations";
    for (unsigned i=0;i<3;i++) e[i]=(LmbReplicaEvidence){.usable=1,.observation_current=1,
        .decode_tok_s=100-i*20,.ttft_seconds=3-i,.measured_at=1000,
        .prompt_tokens=4,.generated_tokens=8,.observation_source=LMB_CAL_SOURCE_SESSION,
        .load={.nodes=2,.max_pending=1,.total_pending=2,.max_age_ms=1000}};
#define LATENCY() lmb_replica_rank(LMB_ROUTE_LATENCY_AWARE,e,3,1000,order)
    /* Interactive preference chooses the first token, not a faster decode
     * that only starts after a longer prefill. Nothing is moved or admitted. */
    assert(!strcmp(LATENCY(),ranked) && order[0]==2 && order[1]==1);
    e[0].load.max_pending=e[0].load.total_pending=0;
    assert(!strcmp(LATENCY(),ranked) && order[0]==0 && order[1]==2);
    e[0].load=e[1].load;
    e[0].ttft_seconds=1.04;
    assert(!strcmp(LATENCY(),ranked) && order[0]==0); /* stable 5% band */
    e[0].ttft_seconds=1.08; e[1].ttft_seconds=1.04;
    assert(!strcmp(LATENCY(),ranked) && order[0]==1); /* no transitive tie */
    e[0].ttft_seconds=3; e[1].ttft_seconds=2;
    double invalid[]={0,-1,NAN,INFINITY};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
        e[2].ttft_seconds=invalid[i];
        assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    }
    e[2].ttft_seconds=1;
    e[2].observation_source=LMB_CAL_SOURCE_PROBE;
    assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    e[2].observation_source=LMB_CAL_SOURCE_UNKNOWN;
    assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    e[2].observation_source=LMB_CAL_SOURCE_SESSION;
    e[2].prompt_tokens++;
    assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    e[2].prompt_tokens--; e[2].generated_tokens++;
    assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    e[2].generated_tokens--;
    e[2].observation_current=0;
    assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    e[2].observation_current=1; e[2].measured_at=699;
    assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    e[2].measured_at=1001;
    assert(!strcmp(LATENCY(),load_only) && order[0]==0);
    e[2].measured_at=1000;
    /* First-token observations don't need eight tokens of decode. Conversely
     * a known decode rate cannot fill in a missing first-token measurement. */
    for (unsigned i=0;i<3;i++) {e[i].generated_tokens=1;e[i].decode_tok_s=NAN;}
    assert(!strcmp(LATENCY(),ranked) && order[0]==2);
    e[2].load.max_age_ms=LMB_REPLICA_LOAD_MAX_AGE_MS+1;
    assert(!strcmp(LATENCY(),"workload_missing_or_expired") && order[0]==0);
    e[2].load=e[1].load; e[1].load.nodes=0;
    assert(!strcmp(LATENCY(),"workload_missing_or_expired") && order[0]==0);
    e[1].usable=0;
    assert(!strcmp(LATENCY(),ranked) && order[0]==2 && order[2]==1);
    e[0].usable=e[2].usable=0;
    assert(!strcmp(LATENCY(),"no_approved_plan"));
#undef LATENCY
}

int main(void) {
    (void)lmb_sign;
    profile_tests(); rank_tests(); load_tests(); latency_tests();
    puts("REPLICA PREFERENCES: PASS (bounded private observations, exact provenance, expiry, missing prices, stable rank)");
    return 0;
}
