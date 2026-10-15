#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/planner/lumabri_elastic_store.h"
#include "src/planner/lumabri_elastic_json.h"
#include <assert.h>
#include <sys/wait.h>

static LmbElasticPolicy fixture(void) {
    LmbElasticPolicy p={.revision=1,.enabled=1,.min_replicas=0,.max_replicas=2,.cooldown=30,
        .idle_seconds=60,.up_rejections=2,.ttft_ms=1000,.max_reserved_bytes=3000,.resident_bytes={1000},
        .route={.id={1},.content={2},.name="olmoe",.tracker="127.0.0.1:47300",.adapter="olmoe",
            .numeric_class="cpu-f32",.revision=1,.numeric_abi=1,.context=128,.max_new=8,.count=1,
            .replicas={{{3},{4},{5}}}},
        .request={.action="start",.models={"olmoe"},.model_count=1,.node_count=1,.context=128,.sessions=2,.max_new=8}};
    memset(p.source_stamp,'a',64); memset(p.request.nodes[0],'b',64);
    assert(lmb_elastic_valid(&p)); return p;
}
static void decision(void) {
    LmbElasticPolicy p=fixture(); LmbDemand d={.started_ms=1000,.captured_ms=61000,.instance={1}};
    const char *why;
    assert(lmb_elastic_choose(&p,&d,61000,1000,&why)==LMB_ELASTIC_HOLD);
    assert(p.idle_since_ms==61000);
    d.captured_ms=121000;
    assert(lmb_elastic_choose(&p,&d,121000,1060,&why)==LMB_ELASTIC_SHRINK);
    d.instance[0]=2;
    assert(lmb_elastic_choose(&p,&d,121000,1060,&why)==LMB_ELASTIC_HOLD && p.idle_since_ms==121000);
    d.captured_ms=181000;
    lmb_demand_loss(&d,180000);
    assert(lmb_elastic_choose(&p,&d,181000,1120,&why)==LMB_ELASTIC_HOLD && !p.idle_since_ms);
    d.loss_count=0; d.captured_ms=241000;
    assert(lmb_elastic_choose(&p,&d,241000,1180,&why)==LMB_ELASTIC_HOLD);
    d.captured_ms=301000;
    memcpy(d.models[0].id,p.route.replicas[0].allocation,32); d.models[0].admitted=1;
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_HOLD && !p.idle_since_ms);
    d.models[0].admitted=0;
    memcpy(d.models[0].id,p.route.id,32);
    lmb_demand_count(&d.models[0],LMB_DEMAND_CAPACITY,301000);
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_HOLD);
    lmb_demand_count(&d.models[0],LMB_DEMAND_CAPACITY,301000);
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_GROW);
    p.last_change=1230;
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_HOLD && !strcmp(why,"cooldown"));
    p.last_change=1241;
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_HOLD && !strcmp(why,"clock_moved_backwards"));
    p.last_change=0; d.captured_ms=299000;
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_HOLD); /* stale pressure */
    p.min_replicas=2;
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_GROW && !strcmp(why,"resident_minimum"));
    p.enabled=0;
    assert(lmb_elastic_choose(&p,&d,301000,1240,&why)==LMB_ELASTIC_HOLD);
    p=fixture(); p.route.count=0; p.resident_bytes[0]=0;
    assert(lmb_elastic_valid(&p));
    d=(LmbDemand){.started_ms=1000,.captured_ms=1001,.instance={2}};
    memcpy(d.models[0].id,p.route.id,32); lmb_demand_count(&d.models[0],LMB_DEMAND_STARTED,1001);
    assert(lmb_elastic_choose(&p,&d,1001,1240,&why)==LMB_ELASTIC_GROW && !strcmp(why,"on_demand_wake"));
    p.last_change=1240; p.cooldown=300;
    assert(lmb_elastic_choose(&p,&d,1001,1241,&why)==LMB_ELASTIC_GROW && !strcmp(why,"on_demand_wake"));
    p.faulted=1;
    assert(lmb_elastic_choose(&p,&d,1001,1240,&why)==LMB_ELASTIC_HOLD);
}
static void recovery(void) {
    LmbElasticPolicy p=fixture(); p.phase=LMB_ELASTIC_PUBLISH_GROW; p.pending_bytes=1500;
    memset(p.request.operation,'c',64); memset(p.request.review,'d',64); p.pending_allocation[0]=6;
    p.target=p.route; p.target.revision++; p.target.replicas[p.target.count++]=(LmbModelReplica){{6},{7},{8}};
    assert(lmb_elastic_valid(&p)); uint64_t bytes; assert(!lmb_elastic_total(&p,&bytes) && bytes==2500);
    LmbBuf encoded={0}; LmbElasticPolicy decoded;
    assert(!lmb_elastic_encode(&p,&encoded)); assert(!lmb_elastic_decode(encoded.p,encoded.len,&decoded)); free(encoded.p);
    LmbElasticPolicy pending=p;
    assert(!lmb_elastic_reconcile(&p,&p.route,1000) && p.phase==LMB_ELASTIC_PUBLISH_GROW);
    LmbModelRoute changed=p.target;
    assert(lmb_elastic_reconcile(&p,&changed,1001)==1);
    assert(!p.phase && !p.pending_bytes && p.route.count==2 && p.resident_bytes[1]==1500 && p.last_change==1001);
    assert(lmb_elastic_valid(&p)); assert(!lmb_elastic_reconcile(&p,&changed,1002) && p.last_change==1001);
    p=pending; changed.context--;
    assert(lmb_elastic_reconcile(&p,&changed,1002)<0 && p.faulted && p.pending_bytes==1500);
    p=fixture(); p.phase=LMB_ELASTIC_PUBLISH_SHRINK; p.pending_bytes=p.resident_bytes[0];
    memcpy(p.pending_allocation,p.route.replicas[0].allocation,32); p.target=p.route; p.target.revision++; p.target.count=0;
    p.max_reserved_bytes=1000; /* retiring is not charged twice before withdrawal */
    assert(lmb_elastic_valid(&p)); assert(!lmb_elastic_total(&p,&bytes) && bytes==1000);
    encoded=(LmbBuf){0}; assert(!lmb_elastic_encode(&p,&encoded)); assert(!lmb_elastic_decode(encoded.p,encoded.len,&decoded)); free(encoded.p);
    changed=p.target; assert(lmb_elastic_reconcile(&p,&changed,2000)==1);
    assert(p.phase==LMB_ELASTIC_RETIRING && !p.route.count && p.pending_bytes==1000);
    assert(lmb_elastic_valid(&p)); assert(!lmb_elastic_total(&p,&bytes) && bytes==1000);
    lmb_elastic_idle(&p); assert(lmb_elastic_valid(&p));
    assert(!lmb_elastic_total(&p,&bytes) && !bytes);
}
static void codec_store(void) {
    LmbElasticPolicy p=fixture(),decoded; LmbBuf b={0};
    assert(!lmb_elastic_encode(&p,&b)); assert(!lmb_elastic_decode(b.p,b.len,&decoded));
    assert(!memcmp(&p,&decoded,sizeof p));
    for (size_t i=0;i<b.len;i++) assert(lmb_elastic_decode(b.p,i,&decoded));
    b.p[40]^=1; assert(lmb_elastic_decode(b.p,b.len,&decoded)); free(b.p);
    char path[]="/tmp/lmb-elastic-test-XXXXXX"; assert(mkdtemp(path));
    int access=lmb_api_access_dir(path,0),root=lmb_elastic_root(access,1); assert(root>=0);
    int lock=lmb_api_access_lock(root); assert(lock>=0);
    pid_t child=fork(); assert(child>=0);
    if (!child) { close(lock); int other=lmb_api_access_lock(root); if (other>=0) close(other); _exit(other<0 ? 0 : 1); }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    int dir=lmb_elastic_dir(root,p.route.id,1); assert(dir>=0);
    assert(lmb_elastic_load(dir,&decoded)==1); assert(!lmb_elastic_save(dir,&p));
    assert(!lmb_elastic_load(dir,&decoded) && !memcmp(&decoded,&p,sizeof p));
    assert(!fchmodat(dir,"policy",0644,0)); assert(lmb_elastic_load(dir,&decoded)<0); assert(lmb_elastic_save(dir,&p));
    assert(!fchmodat(dir,"policy",0600,0));
    assert(!symlinkat("policy",dir,".pending")); assert(lmb_elastic_save(dir,&p)); assert(!unlinkat(dir,".pending",0));
    int fd=openat(dir,".pending",O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0); assert(write(fd,"crash",5)==5); close(fd);
    assert(!lmb_elastic_save(dir,&p));
    assert(!linkat(dir,"policy",dir,"alias",0)); assert(lmb_elastic_load(dir,&decoded)<0); assert(!unlinkat(dir,"alias",0));
    assert(!unlinkat(dir,"policy",0)); assert(!mkfifoat(dir,"policy",0600)); assert(lmb_elastic_load(dir,&decoded)<0);
    assert(!unlinkat(dir,"policy",0)); close(dir);
    char id[65]; lmb_hex(id,p.route.id,32); assert(!unlinkat(root,id,AT_REMOVEDIR));
    assert(!unlinkat(root,"access.lock",0)); close(lock); close(root);
    assert(!unlinkat(access,"elastic",AT_REMOVEDIR)); close(access); assert(!rmdir(path));
}
static void json(void) {
    const char *id="0100000000000000000000000000000000000000000000000000000000000000";
    char text[2048]; LmbElasticPolicy p; int configure;
    snprintf(text,sizeof text,"{\"action\":\"configure\",\"id\":\"%s\",\"revision\":0,\"route_revision\":1,\"enabled\":true,"
        "\"min_replicas\":0,\"max_replicas\":2,\"cooldown_seconds\":30,\"idle_seconds\":60,\"up_rejections\":2,"
        "\"ttft_ms\":1000,\"max_reserved_bytes\":\"3000\",\"source_model\":\"olmoe\",\"nodes\":[\"%s\"],"
        "\"context\":128,\"sessions\":2,\"max_new\":8,\"adopt_allocations\":[\"%s\"]}",id,id,id);
    assert(!lmb_elastic_parse(text,strlen(text),&p,&configure) && configure && p.route.count==1);
    assert(!p.revision && p.max_reserved_bytes==3000);
    for (size_t n=0;n<strlen(text);n++) assert(lmb_elastic_parse(text,n,&p,&configure));
    snprintf(text,sizeof text,"{\"action\":\"disable\",\"id\":\"%s\",\"revision\":1}",id);
    assert(!lmb_elastic_parse(text,strlen(text),&p,&configure) && !configure);
    snprintf(text,sizeof text,"{\"action\":\"resume\",\"id\":\"%s\",\"revision\":1}",id);
    assert(!lmb_elastic_parse(text,strlen(text),&p,&configure) && configure==2);
    snprintf(text,sizeof text,"{\"action\":\"reconcile\",\"id\":\"%s\",\"revision\":1}",id);
    assert(!lmb_elastic_parse(text,strlen(text),&p,&configure) && configure==3);
    snprintf(text,sizeof text,"{\"action\":\"resume\",\"id\":\"%s\",\"revision\":4294967295}",id);
    assert(lmb_elastic_parse(text,strlen(text),&p,&configure));
    snprintf(text,sizeof text,"{\"action\":\"disable\",\"id\":\"%s\",\"revision\":true}",id);
    assert(lmb_elastic_parse(text,strlen(text),&p,&configure));
    snprintf(text,sizeof text,"{\"action\":\"disable\",\"id\":\"%s\",\"revision\":1,\"command\":\"ignored?\"}",id);
    assert(lmb_elastic_parse(text,strlen(text),&p,&configure));
    p=fixture(); p.max_replicas=0; assert(!lmb_elastic_valid(&p));
    p=fixture(); p.max_reserved_bytes=999; assert(!lmb_elastic_valid(&p));
    p=fixture(); p.request.node_count=2; strcpy(p.request.nodes[1],p.request.nodes[0]); assert(!lmb_elastic_valid(&p));
}
int main(void) {
    (void)lmb_sign; decision(); recovery(); codec_store(); json();
    puts("ELASTIC POLICY: PASS (demand, cooldown, fresh idle evidence, scale to zero, exact publication recovery, budgets, private intent store, strict configuration)");
    return 0;
}
