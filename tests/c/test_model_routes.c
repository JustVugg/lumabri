#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/runtime/lumabri_model_routes.h"
#include <assert.h>
#include <sys/wait.h>

static void joint_registry(void) {
    char temporary[]="/tmp/lmb-joint-routes-XXXXXX"; assert(mkdtemp(temporary));
    int access=lmb_api_access_dir(temporary,0),dir=lmb_route_dir(access,1); assert(dir>=0);
    LmbModelRoute pair[2]={
        {.id={1},.content={2},.name="first",.tracker="127.0.0.1:47300",.adapter="olmoe",
         .numeric_class="cpu-f32",.revision=1,.numeric_abi=1,.context=128,.max_new=8,.count=1,.replicas={{{3},{4},{5}}}},
        {.id={6},.content={7},.name="second",.tracker="127.0.0.1:47300",.adapter="olmoe",
         .numeric_class="cpu-f32",.revision=1,.numeric_abi=1,.context=128,.max_new=8,.count=1,.replicas={{{8},{9},{10}}}}
    };
    /* Existing per-model files migrate on the first successful publication.
     * Retain the legacy file as evidence; it never overrides the new snapshot. */
    LmbBuf legacy={0}; assert(!lmb_route_encode(&pair[0],&legacy));
    char name[65]; lmb_hex(name,pair[0].id,32);
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0);
    assert(write(fd,legacy.p,legacy.len)==(ssize_t)legacy.len); close(fd); free(legacy.p);
    LmbRouteRegistry *s=calloc(1,sizeof *s); assert(s);
    assert(!lmb_route_registry_read(dir,s) && s->count==1);
    assert(!lmb_route_save(dir,&pair[1],0));
    uint32_t expected[2]={1,1}; pair[0].revision=pair[1].revision=2;
    pair[1].content[0]^=1;
    assert(lmb_route_save_many(dir,pair,expected,2)==LMB_ROUTE_CONFLICT);
    assert(!lmb_route_registry_read(dir,s) && s->count==2 && s->routes[0].revision==1 && s->routes[1].revision==1);
    pair[1].content[0]^=1;
    int lock=lmb_api_access_lock(dir); assert(lock>=0);
    assert(lmb_route_save_many(dir,pair,expected,2)==LMB_ROUTE_BUSY); close(lock);
    assert(!lmb_route_save_many(dir,pair,expected,2));
    assert(lmb_route_save_many(dir,pair,expected,2)==LMB_ROUTE_CONFLICT);
    assert(!lmb_route_registry_read(dir,s) && s->routes[0].revision==2 && s->routes[1].revision==2);
    LmbBuf encoded={0}; assert(!lmb_route_registry_encode(s,&encoded));
    for (size_t i=0;i<encoded.len;i++) assert(lmb_route_registry_decode(encoded.p,i,s));
    encoded.p[30]^=1; assert(lmb_route_registry_decode(encoded.p,encoded.len,s)); encoded.p[30]^=1;
    assert(!lmb_route_registry_decode(encoded.p,encoded.len,s)); free(encoded.p);
    /* Deterministically open before rename and read after it. The old inode
     * has zero links but still contains a valid complete snapshot. */
    fd=openat(dir,".registry",O_RDONLY|O_NOFOLLOW); assert(fd>=0);
    expected[0]=pair[0].revision++; expected[1]=pair[1].revision++;
    assert(!lmb_route_save_many(dir,pair,expected,2));
    struct stat replaced; assert(!fstat(fd,&replaced) && replaced.st_nlink==0);
    assert(!lmb_route_registry_read_fd(fd,s) && s->routes[0].revision==2 && s->routes[1].revision==2); close(fd);
    /* Readers survive publication and never see half of the portfolio. */
    int ready[2]; assert(!pipe(ready)); pid_t child=fork(); assert(child>=0);
    if (!child) {
        close(ready[0]); assert(write(ready[1],"R",1)==1); close(ready[1]);
        for (unsigned i=0;i<400;i++) {
            assert(!lmb_route_registry_read(dir,s) && s->count==2);
            assert(s->routes[0].revision==s->routes[1].revision);
        }
        _exit(0);
    }
    close(ready[1]); char byte; assert(read(ready[0],&byte,1)==1); close(ready[0]);
    for (unsigned i=0;i<40;i++) {
        expected[0]=pair[0].revision++; expected[1]=pair[1].revision++;
        assert(!lmb_route_save_many(dir,pair,expected,2));
    }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    /* Crash before publication leaves only an incomplete transaction. */
    fd=openat(dir,".transaction",O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0);
    assert(write(fd,"partial",7)==7); close(fd);
    assert(!lmb_route_registry_read(dir,s) && s->routes[0].revision==pair[0].revision);
    expected[0]=pair[0].revision++; expected[1]=pair[1].revision++;
    assert(!lmb_route_save_many(dir,pair,expected,2));
    assert(!lmb_route_remove(dir,pair[0].id,pair[0].revision));
    assert(!lmb_route_remove(dir,pair[1].id,pair[1].revision));
    assert(!lmb_route_registry_read(dir,s) && s->count==0); /* legacy must not revive */
    assert(lmb_route_load(dir,pair[0].id,&pair[0])==LMB_ROUTE_MISSING);
    assert(!unlinkat(dir,".registry",0)); assert(!mkfifoat(dir,".registry",0600));
    assert(lmb_route_registry_read(dir,s)==LMB_ROUTE_UNSAFE); assert(!unlinkat(dir,".registry",0));
    assert(!symlinkat(name,dir,".registry"));
    assert(lmb_route_registry_read(dir,s)==LMB_ROUTE_UNSAFE); assert(!unlinkat(dir,".registry",0));
    free(s); assert(!unlinkat(dir,name,0)); assert(!unlinkat(dir,"access.lock",0)); close(dir);
    assert(!unlinkat(access,"models",AT_REMOVEDIR)); close(access); assert(!rmdir(temporary));
}

int main(void) {
    (void)lmb_sign;
    _Static_assert(sizeof(LmbModelReplica)==96,"replicas use byte arrays only");
    LmbModelRoute route={.id={1},.content={2},.name="olmoe",.tracker="127.0.0.1:47300",.adapter="olmoe",
        .numeric_class="cpu-f32",.revision=1,.numeric_abi=1,.context=128,.max_new=8,.count=2,
        .replicas={{{3},{4},{5}},{{6},{7},{8}}}};
    assert(lmb_route_valid(&route)); LmbBuf data={0};
    assert(!lmb_route_encode(&route,&data)); assert(data.len<LMB_ROUTE_BYTES);
    LmbModelRoute decoded;
    assert(!lmb_route_decode(data.p,data.len,&decoded));
    assert(decoded.policy==LMB_ROUTE_ORDERED);
    data.p[7]='1';
    assert(!lmb_route_decode(data.p,data.len-4,&decoded) && decoded.policy==LMB_ROUTE_ORDERED);
    assert(lmb_route_decode(data.p,data.len,&decoded)); /* no ambiguous trailer */
    data.p[7]='2';
    assert(!lmb_route_decode(data.p,data.len,&decoded));
    LmbBuf encoded={0}; assert(!lmb_route_encode(&decoded,&encoded));
    assert(encoded.len==data.len && !memcmp(encoded.p,data.p,data.len)); free(encoded.p);
    for (size_t n=0; n<data.len; n++) assert(lmb_route_decode(data.p,n,&decoded));
    unsigned char previous=data.p[76]; data.p[76]=0xff; assert(lmb_route_decode(data.p,data.len,&decoded)); data.p[76]=previous;
    free(data.p);
    char temporary[]="/tmp/lmb-model-routes-XXXXXX"; assert(mkdtemp(temporary));
    int access=lmb_api_access_dir(temporary,0); assert(access>=0);
    int dir=lmb_route_dir(access,1); assert(dir>=0);
    assert(lmb_route_load(dir,route.id,&decoded)==LMB_ROUTE_MISSING);
    assert(!lmb_route_save(dir,&route,0)); assert(!lmb_route_load(dir,route.id,&decoded));
    assert(decoded.count==2 && !strcmp(decoded.name,route.name) && !memcmp(decoded.content,route.content,32));
    assert(lmb_route_save(dir,&route,0)==LMB_ROUTE_CONFLICT);
    LmbModelRoute changed=route; changed.revision=2; changed.content[0]^=1;
    assert(lmb_route_save(dir,&changed,1)==LMB_ROUTE_CONFLICT);
    changed=route; changed.revision=2; changed.numeric_abi++;
    assert(lmb_route_save(dir,&changed,1)==LMB_ROUTE_CONFLICT);
    changed=route; changed.revision=2; changed.count=1;
    changed.policy=LMB_ROUTE_OBSERVED_DECODE;
    assert(!lmb_route_save(dir,&changed,1));
    assert(!lmb_route_load(dir,route.id,&decoded) && decoded.policy==LMB_ROUTE_OBSERVED_DECODE);
    assert(lmb_route_save(dir,&changed,1)==LMB_ROUTE_CONFLICT);
    route=changed; changed.id[0]=9; changed.revision=1;
    assert(lmb_route_save(dir,&changed,0)==LMB_ROUTE_CONFLICT); /* duplicate name */
    strcpy(changed.name,"other"); assert(!lmb_route_save(dir,&changed,0));
    uint8_t ids[LMB_ROUTE_MAX][32]; size_t count;
    assert(!lmb_route_list(dir,ids,&count) && count==2);
    assert(!lmb_route_list(dir,ids,&count) && count==2);
    assert(lmb_route_remove(dir,route.id,1)==LMB_ROUTE_CONFLICT);
    assert(!lmb_route_remove(dir,route.id,2));
    assert(lmb_route_load(dir,route.id,&decoded)==LMB_ROUTE_MISSING);
    route.revision=1; assert(!lmb_route_save(dir,&route,0));
    const char *hex=".registry";
    assert(!fchmodat(dir,hex,0644,0));
    assert(lmb_route_load(dir,route.id,&decoded)==LMB_ROUTE_UNSAFE);
    assert(!fchmodat(dir,hex,0600,0));
    assert(!linkat(dir,hex,dir,"hardlink",0));
    assert(lmb_route_load(dir,route.id,&decoded)==LMB_ROUTE_UNSAFE);
    assert(!unlinkat(dir,"hardlink",0));
    assert(!symlinkat(hex,dir,".transaction"));
    route.revision=2;
    assert(lmb_route_save(dir,&route,1)==LMB_ROUTE_UNSAFE);
    assert(!unlinkat(dir,".transaction",0));
    assert(!lmb_route_save(dir,&route,1));
    for (unsigned i=2;i<LMB_ROUTE_MAX;i++) {
        changed.id[0]=(uint8_t)(i+10);
        snprintf(changed.name,sizeof changed.name,"model-%u",i);
        assert(!lmb_route_save(dir,&changed,0));
    }
    changed.id[0]=90; strcpy(changed.name,"overflow");
    assert(lmb_route_save(dir,&changed,0)==LMB_ROUTE_QUOTA);
    assert(!lmb_route_list(dir,ids,&count) && count==LMB_ROUTE_MAX);
    assert(!unlinkat(dir,".registry",0));
    assert(!unlinkat(dir,"access.lock",0)); close(dir);
    assert(!unlinkat(access,"models",AT_REMOVEDIR)); close(access); assert(!rmdir(temporary));
    joint_registry();
    puts("MODEL ROUTES: PASS (exact identities, immutable semantics, revision-fenced atomic portfolios, legacy migration, concurrent reads and crash-before-publication recovery)");
    return 0;
}
