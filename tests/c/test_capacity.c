#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/planner/lumabri_capacity_store.h"
#include <assert.h>
#include <sys/wait.h>

static LmbCapacity fixture(void) {
    LmbCapacity r={.fingerprint={1},.models=2,.clients=2,.rounds=3,.max_new=8,.count=6,.stable=1,.measured_at=1000};
    for (unsigned m=0;m<2;m++) {
        r.allocations[m][0]=1+m; r.contents[m][0]=3+m; r.contracts[m][0]=5+m;
        r.numeric_abi[m]=1; strcpy(r.numeric_class[m],"cpu-f32");
    }
    for (unsigned i=0;i<6;i++) r.samples[i]=(LmbCapacitySample){.model=i%2,.round=i/2,.status=200,
        .complete=1,.timing_known=1,.prompt_tokens=12,.generated_tokens=8,.ttft=.2+i*.01,.gap_p95=.1,.completion=1};
    assert(lmb_capacity_valid(&r)); return r;
}
static void envelope_tests(void) {
    LmbCapacity a=fixture(),b=a;
    LmbCapacitySummary s=lmb_capacity_summary(&a,0);
    assert(s.requests==3 && s.completed==3 && s.measured==3 && s.ttft_p95==a.samples[4].ttft);
    assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1000,300,1,.2),"observed_workload_passed"));
    assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1301,300,1,.2),"expired"));
    assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,999,300,1,.2),"expired"));
    b.fingerprint[0]++;
    assert(!strcmp(lmb_capacity_evaluate(&a,b.fingerprint,1000,300,1,.2),"configuration_changed"));
    a.stable=0; assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1000,300,1,.2),"configuration_changed"));
    a=fixture(); a.samples[0]=(LmbCapacitySample){.status=429,.completion=.01};
    s=lmb_capacity_summary(&a,0); assert(s.requests==3 && s.completed==2 && s.rejected==1);
    assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1000,300,1,.2),"requests_rejected_or_failed"));
    a.samples[0].status=503; s=lmb_capacity_summary(&a,0); assert(s.failed==1 && !s.rejected);
    a=fixture(); a.samples[0]=(LmbCapacitySample){.status=200,.complete=1,.completion=.5};
    assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1000,300,1,.2),"timing_unavailable"));
    a=fixture(); assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1000,300,.01,.2),"latency_target_missed"));
    assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1000,300,1,NAN),"invalid_evidence_or_limits"));
    a.rounds=2; a.count=4; assert(!strcmp(lmb_capacity_evaluate(&a,a.fingerprint,1000,300,1,.2),"insufficient_rounds"));
    a=fixture(); b=a; b.allocations[0][0]=10; assert(lmb_capacity_comparable(&a,&b));
    b.clients=1; assert(!lmb_capacity_comparable(&a,&b));
    b=a; b.contents[0][0]++; assert(!lmb_capacity_comparable(&a,&b));
    b=a; b.contracts[1][0]++; assert(!lmb_capacity_comparable(&a,&b));
    b=a; b.samples[1].prompt_tokens++; assert(!lmb_capacity_comparable(&a,&b));
    b=a; b.samples[2].generated_tokens--; assert(!lmb_capacity_comparable(&a,&b));
    b=a; b.samples[0].gap_p95=INFINITY; assert(!lmb_capacity_valid(&b));
    b=a; b.samples[0].model=1; assert(!lmb_capacity_valid(&b));
    b=a; b.samples[0].ttft=2; assert(!lmb_capacity_valid(&b));
    b=a; b.clients=9; assert(!lmb_capacity_valid(&b));
}
static void key_tests(void) {
    LmbCalKey k={0}; memset(k.model_root,'a',64); strcpy(k.adapter,"olmoe"); k.adapter_abi=1;
    strcpy(k.numeric_class,"cpu-f32"); strcpy(k.build_id,"runtime"); strcpy(k.plan_kind,"segment");
    k.nodes=1; k.context=128; k.sessions=1;
    strcpy(k.node_id[0],"node"); strcpy(k.node_hardware_id[0],"hardware");
    strcpy(k.node_build_id[0],"engine"); strcpy(k.node_backend[0],"cpu"); k.layer_end[0]=2; k.threads[0]=4;
    uint8_t a[32],b[32]; assert(!lmb_capacity_key_hash(&k,a)); assert(!lmb_capacity_key_hash(&k,b) && !memcmp(a,b,32));
    k.threads[0]++; assert(!lmb_capacity_key_hash(&k,b) && memcmp(a,b,32)); k.threads[0]--;
    k.workload[0]=(LmbWorkloadFacts){.known=1,.allocations=1,.compute_policy=LMB_COMPUTE_LOCAL_FIFO,
        .reserved_bytes=1024,.allocation_set={1}};
    assert(!lmb_capacity_key_hash(&k,b) && memcmp(a,b,32)); memcpy(a,b,32);
    k.workload[0].allocation_set[0]++; assert(!lmb_capacity_key_hash(&k,b) && memcmp(a,b,32));
    k.workload[0].active=1; assert(lmb_capacity_key_hash(&k,b));
}
static void cost_tests(void) {
    LmbCapacityCost costs[3]={{.known=1},{.known=1},{.known=1}};
    LmbResourceFacts a={.known=LMB_FACT_PRICE,.currency="EUR",.price_micro_per_hour=100};
    lmb_capacity_cost_add(&costs[0],&a); lmb_capacity_cost_add(&costs[0],&a);
    lmb_capacity_cost_add(&costs[1],&a); lmb_capacity_cost_add(&costs[2],&a);
    assert(costs[0].known && costs[0].micro_per_hour==200);
    int eligible[3]={1,1,1},prices;
    assert(lmb_capacity_choose(costs,eligible,3,&prices)==1 && prices);
    costs[2].known=0; assert(lmb_capacity_choose(costs,eligible,3,&prices)==0 && !prices);
    eligible[2]=0; assert(lmb_capacity_choose(costs,eligible,3,&prices)==1 && prices);
    eligible[2]=1; costs[2].known=1; strcpy(costs[2].currency,"USD");
    assert(lmb_capacity_choose(costs,eligible,3,&prices)==0 && !prices);
    a.known=0; lmb_capacity_cost_add(&costs[0],&a); assert(!costs[0].known);
    a.known=LMB_FACT_PRICE; lmb_capacity_cost_add(&costs[2],&a); assert(!costs[2].known);
    costs[1].micro_per_hour=UINT64_MAX; lmb_capacity_cost_add(&costs[1],&a); assert(!costs[1].known);
    eligible[0]=eligible[1]=eligible[2]=0; assert(lmb_capacity_choose(costs,eligible,3,&prices)==-1);
}
static void store_tests(void) {
    LmbCapacity a=fixture(),b; LmbBuf bytes={0}; assert(!lmb_capacity_encode(&a,&bytes));
    assert(!lmb_capacity_decode(bytes.p,bytes.len,&b) && lmb_capacity_comparable(&a,&b));
    for (size_t i=0;i<bytes.len;i++) assert(lmb_capacity_decode(bytes.p,i,&b));
    bytes.p[20]^=1; assert(lmb_capacity_decode(bytes.p,bytes.len,&b)); free(bytes.p);
    char path[]="/tmp/lmb-capacity-XXXXXX"; assert(mkdtemp(path));
    int parent=lmb_api_access_dir(path,0); assert(parent>=0);
    int dir=lmb_capacity_dir(parent,1); assert(dir>=0);
    int lock=lmb_capacity_lock(dir); assert(lock>=0);
    pid_t child=fork(); assert(child>=0);
    if (!child) { close(lock); int other=lmb_capacity_lock(dir); if (other>=0) close(other); _exit(other<0 ? 0 : 1); }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!lmb_capacity_save(dir,"first",&a)); assert(lmb_capacity_save(dir,"first",&a));
    assert(!lmb_capacity_load(dir,"first",&b) && lmb_capacity_comparable(&a,&b));
    assert(!fchmodat(dir,"first",0644,0)); assert(lmb_capacity_load(dir,"first",&b)); assert(!fchmodat(dir,"first",0600,0));
    assert(!symlinkat("first",dir,"symlink")); assert(lmb_capacity_load(dir,"symlink",&b)); assert(lmb_capacity_save(dir,"symlink",&a));
    assert(!unlinkat(dir,"symlink",0));
    assert(!mkfifoat(dir,"fifo",0600)); assert(lmb_capacity_load(dir,"fifo",&b)); assert(!unlinkat(dir,"fifo",0));
    assert(!symlinkat("first",dir,".pending")); assert(lmb_capacity_save(dir,"second",&a)); assert(!unlinkat(dir,".pending",0));
    assert(!linkat(dir,"first",dir,".pending",0)); /* crash between link and cleanup */
    assert(lmb_capacity_load(dir,"first",&b));
    assert(!lmb_capacity_save(dir,"recovered",&a));
    assert(!lmb_capacity_load(dir,"first",&b)); assert(!lmb_capacity_remove(dir,"recovered"));
    for (unsigned i=1;i<LMB_CAPACITY_RECORDS;i++) { char name[32]; snprintf(name,sizeof name,"record-%u",i); assert(!lmb_capacity_save(dir,name,&a)); }
    assert(lmb_capacity_save(dir,"too-many",&a));
    assert(!linkat(dir,"first",dir,".pending",0)); close(lock);
    lock=lmb_capacity_lock(dir); assert(lock>=0); assert(!lmb_capacity_load(dir,"first",&b));
    for (unsigned i=1;i<LMB_CAPACITY_RECORDS;i++) { char name[32]; snprintf(name,sizeof name,"record-%u",i); assert(!unlinkat(dir,name,0)); }
    assert(!lmb_capacity_remove(dir,"first")); assert(lmb_capacity_remove(dir,"../first"));
    assert(!unlinkat(dir,"probe.lock",0)); close(lock); close(dir);
    assert(!unlinkat(parent,"capacity",AT_REMOVEDIR)); close(parent); assert(!rmdir(path));
}
int main(void) {
    (void)lmb_sign; envelope_tests(); key_tests(); cost_tests(); store_tests();
    puts("CAPACITY ENVELOPES: PASS (joint provenance, all outcomes, per-model tails, exact workload, expiry, private bounded records)");
    return 0;
}
