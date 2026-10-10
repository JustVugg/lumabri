#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/planner/lumabri_portfolio_policy_store.h"
#include "src/planner/lumabri_portfolio_policy_json.h"
#include <assert.h>
#include <sys/wait.h>

static LmbPortfolioPolicy fixture(void) {
    LmbPortfolioPolicy p={.revision=1,.enabled=1,.models=2,.candidates=2,.current=0,.ttft_ms=1000,.gap_ms=200,
        .max_age=300,.cooldown=30,.horizon=3600,.min_saving_bps=1000,.ceiling_micro_per_hour=10000,
        .switch_cost_micro=10,.currency="EUR",.tracker="127.0.0.1:47300",.reason="test"};
    for (unsigned i=0;i<2;i++) {
        snprintf(p.names[i],sizeof p.names[i],"record-%u",i);
        p.records[i][0]=p.model_ids[i][0]=p.routes[i][0]=i+1; p.route_revisions[i]=1;
    }
    assert(lmb_portfolio_policy_valid(&p)); return p;
}
static void decision_tests(void) {
    LmbPortfolioPolicy p=fixture(); int passing[2]={1,1}; const char *reason;
    LmbCapacityCost costs[2]={{.known=1,.micro_per_hour=1000,.currency="EUR"},
                            {.known=1,.micro_per_hour=800,.currency="EUR"}};
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==1);
    p.last_change=990; assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"cooldown"));
    p.last_change=1001; assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"clock_moved_backwards"));
    p.last_change=0; p.last_check=1001;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"clock_moved_backwards"));
    p.last_check=0; p.min_saving_bps=3000;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"saving_below_hysteresis"));
    p.min_saving_bps=1000; p.switch_cost_micro=200;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"switch_cost_not_recovered"));
    p.switch_cost_micro=10; p.horizon=1;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1);
    p.horizon=3600; costs[1].known=0;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1);
    costs[1].known=1; strcpy(costs[1].currency,"USD");
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1);
    strcpy(costs[1].currency,"EUR"); costs[1].micro_per_hour=1000;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"current_portfolio_preferred"));
    passing[0]=0; costs[1].micro_per_hour=1200;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==1 && !strcmp(reason,"restore_observed_requirements"));
    p.ceiling_micro_per_hour=1100;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"no_measured_candidate_within_budget"));
    p.enabled=0; assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"disabled"));
    p.enabled=1; p.faulted=1;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1 && !strcmp(reason,"manual_reconfiguration_required"));
    p=fixture(); p.ceiling_micro_per_hour=UINT64_C(32000000000000); p.horizon=86400; p.min_saving_bps=9999;
    costs[0].micro_per_hour=p.ceiling_micro_per_hour; costs[1].micro_per_hour=0; passing[0]=1;
    assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==1);
    costs[1].micro_per_hour=UINT64_MAX; assert(lmb_portfolio_policy_choose(&p,passing,costs,1000,&reason)==-1);
}
static LmbModelRoute route(unsigned i) {
    LmbModelRoute r={.revision=1,.id={0},.content={10},.name="one",.tracker="127.0.0.1:47300",.adapter="olmoe",
        .numeric_class="cpu-f32",.numeric_abi=1,.context=128,.max_new=8,.count=1};
    r.id[0]=i+1; r.replicas[0]=(LmbModelReplica){.allocation={1},.root={2},.host_key={3}};
    if (i) strcpy(r.name,"two");
    assert(lmb_route_valid(&r)); return r;
}
static void recovery_tests(void) {
    LmbPortfolioPolicy p=fixture(); LmbRouteRegistry s={.count=2,.routes={route(0),route(1)}};
    for (unsigned i=0;i<2;i++) assert(!lmb_policy_route_hash(&s.routes[i],p.routes[i]));
    assert(!lmb_policy_reconcile(&p,&s,1000) && !p.faulted);
    p.pending=2;
    for (unsigned i=0;i<2;i++) {
        LmbModelRoute target=s.routes[i]; target.revision++; target.replicas[0].allocation[0]=4;
        assert(!lmb_policy_route_hash(&target,p.targets[i]));
    }
    LmbPortfolioPolicy pending=p;
    assert(!lmb_policy_reconcile(&p,&s,1000) && !p.pending && !p.faulted && p.current==0);
    assert(!strcmp(p.reason,"unpublished_intent_cleared"));
    p=pending; for (unsigned i=0;i<2;i++) { s.routes[i].revision++; s.routes[i].replicas[0].allocation[0]=4; }
    assert(lmb_policy_reconcile(&p,&s,1000)==1 && p.current==1 && p.last_change==1000 && !p.pending);
    assert(p.route_revisions[0]==2 && p.route_revisions[1]==2);
    assert(!lmb_policy_reconcile(&p,&s,1001) && p.last_change==1000); /* no duplicate acknowledgement */
    p=pending; s.routes[1]=route(1);
    assert(lmb_policy_reconcile(&p,&s,1000)==-1 && p.faulted); /* partial or external mutation */
    p=pending; s.routes[1].revision=2; s.routes[1].replicas[0].allocation[0]=4; s.routes[1].context=64;
    assert(lmb_policy_reconcile(&p,&s,1000)==-1 && p.faulted); /* not only allocation/revision */
}
static void store_tests(void) {
    LmbPortfolioPolicy p=fixture(),copy; LmbBuf b={0};
    assert(!lmb_policy_encode(&p,&b)); assert(!lmb_policy_decode(b.p,b.len,&copy));
    assert(!memcmp(&p,&copy,sizeof p));
    for (size_t i=0;i<b.len;i++) assert(lmb_policy_decode(b.p,i,&copy));
    b.p[50]^=1; assert(lmb_policy_decode(b.p,b.len,&copy)); free(b.p);
    char path[]="/tmp/lmb-policy-XXXXXX"; assert(mkdtemp(path)); int parent=lmb_api_access_dir(path,0); assert(parent>=0);
    int dir=lmb_policy_dir(parent,1); assert(dir>=0); int lock=lmb_policy_lock(dir); assert(lock>=0);
    pid_t child=fork(); assert(child>=0);
    if (!child) { close(lock); int other=lmb_policy_lock(dir); if (other>=0) close(other); _exit(other<0 ? 0 : 1); }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(lmb_policy_load(dir,&copy)==1); assert(!lmb_policy_save(dir,&p)); assert(!lmb_policy_load(dir,&copy));
    assert(!memcmp(&p,&copy,sizeof p));
    assert(!fchmodat(dir,"policy",0644,0)); assert(lmb_policy_load(dir,&copy)<0); assert(lmb_policy_save(dir,&p));
    assert(!fchmodat(dir,"policy",0600,0));
    assert(!symlinkat("policy",dir,".transaction")); assert(lmb_policy_save(dir,&p)); assert(!unlinkat(dir,".transaction",0));
    int fd=openat(dir,".transaction",O_CREAT|O_EXCL|O_WRONLY,0600); assert(fd>=0); assert(write(fd,"broken",6)==6); close(fd);
    p.last_check=123; assert(!lmb_policy_save(dir,&p)); assert(!lmb_policy_load(dir,&copy) && copy.last_check==123);
    assert(!linkat(dir,"policy",dir,"alias",0)); assert(lmb_policy_load(dir,&copy)<0); assert(!unlinkat(dir,"alias",0));
    assert(!unlinkat(dir,"policy",0)); assert(!mkfifoat(dir,"policy",0600)); assert(lmb_policy_load(dir,&copy)<0);
    assert(!unlinkat(dir,"policy",0)); assert(!symlinkat("policy.lock",dir,"policy")); assert(lmb_policy_load(dir,&copy)<0);
    assert(!unlinkat(dir,"policy",0)); assert(!unlinkat(dir,"policy.lock",0)); close(lock); close(dir);
    assert(!unlinkat(parent,"portfolio-policy",AT_REMOVEDIR)); close(parent); assert(!rmdir(path));
}
static void json_tests(void) {
    LmbPortfolioPolicy p; int configure;
    const char *disable="{\"action\":\"disable\",\"revision\":1}";
    assert(!lmb_policy_parse(disable,strlen(disable),&p,&configure) && !configure && p.revision==1);
    const char *exhausted="{\"action\":\"disable\",\"revision\":4294967295}";
    assert(!lmb_policy_parse(exhausted,strlen(exhausted),&p,&configure) && !configure && p.revision==UINT32_MAX);
    const char *bad[]={"{}","{\"action\":\"disable\",\"revision\":0}","{\"action\":\"disable\",\"revision\":1,\"enabled\":false}",
        "{\"action\":\"disable\",\"revision\":1,\"revision\":1}","{\"action\":\"disable\",\"revision\":1,\"cloud\":true}"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) assert(lmb_policy_parse(bad[i],strlen(bad[i]),&p,&configure));
    const char *good="{\"action\":\"configure\",\"revision\":0,\"records\":[\"first\",\"second\"],\"current\":0,"
        "\"models\":[{\"id\":\"0100000000000000000000000000000000000000000000000000000000000000\",\"revision\":1}],"
        "\"ttft_ms\":1000,\"gap_ms\":200,\"max_age_seconds\":300,\"cooldown_seconds\":60,\"horizon_seconds\":3600,"
        "\"min_saving_bps\":1000,\"currency\":\"EUR\",\"ceiling_micro_per_hour\":\"2000000\",\"switch_cost_micro\":\"500\",\"enabled\":true}";
    assert(!lmb_policy_parse(good,strlen(good),&p,&configure) && configure && p.models==1 && p.candidates==2);
    assert(p.ceiling_micro_per_hour==2000000 && p.switch_cost_micro==500 && p.enabled);
    char money[128]; LmbJson j; LmbJsonToken tokens[4]; uint64_t amount;
    snprintf(money,sizeof money,"\"%llu\"",(unsigned long long)UINT64_MAX);
    assert(!lmb_json_parse(&j,money,strlen(money),tokens,4)); assert(lmb_policy_money(&j,0,UINT64_C(32000000000000),&amount));
}
int main(void) {
    (void)lmb_sign; decision_tests(); recovery_tests(); store_tests(); json_tests();
    puts("PORTFOLIO POLICY: PASS (bounded cost, hysteresis, cooldown, exact crash reconciliation, private durable state, strict schema)");
    return 0;
}
