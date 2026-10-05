#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "src/runtime/lumabri_model_routes.h"
#include <assert.h>

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
    char hex[65]; lmb_hex(hex,route.id,32);
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
    for (size_t i=0;i<count;i++) { char name[65]; lmb_hex(name,ids[i],32); assert(!unlinkat(dir,name,0)); }
    assert(!unlinkat(dir,"access.lock",0)); close(dir);
    assert(!unlinkat(access,"models",AT_REMOVEDIR)); close(access); assert(!rmdir(temporary));
    puts("MODEL ROUTES: PASS (exact replica identities, immutable content/numeric semantics, revisions and atomic private storage)");
    return 0;
}
