#define _GNU_SOURCE
#define LMB_TRACKER_NO_ENTRY
#include "tracker.c"
#include <assert.h>
#ifndef _WIN32
#include <ftw.h>
#include <sys/wait.h>
#endif

static void forget_caches(void) {
    for (int i=0;i<g_ntruth;i++) free(g_truth[i].hash);
    memset(g_truth,0,sizeof g_truth); g_ntruth=0; g_truth_cursor=0;
    memset(g_models,0,sizeof g_models); g_model_cursor=0;
    memset(g_bindings,0,sizeof g_bindings); g_nbindings=0; g_binding_cursor=0;
}
static LmbModelIdentity identity(const char *name,unsigned value) {
    LmbModelIdentity out={0}; snprintf(out.model,sizeof out.model,"%s",name);
    uint8_t hash[32]={0}; hash[0]=(uint8_t)value;
    LmbModelItem item={"weights",1,1,hash}; assert(!lmb_model_root(name,&item,1,out.root));
    return out;
}
static int claim_model(const LmbModelIdentity *id,unsigned value,int full) {
    PFile file={"weights",1}; uint8_t bytes[32]={0}; bytes[0]=(uint8_t)value;
    uint8_t *hash=bytes; uint32_t count=1;
    return model_identity_check(id->model,id,&file,full ? 1u : 0u,&hash,&count);
}
static int claim_truth(const char *model,const char *path,unsigned value,const uint8_t *secret) {
    uint8_t *hash=calloc(1,32),sig[64]={0}; assert(hash); hash[0]=(uint8_t)value;
    if (secret) {
        size_t n=0; uint8_t *msg=lmb_truth_msg(model,path,LMB_HASH_CHUNK,1,hash,1,&n); assert(msg);
        lmb_sign(sig,msg,n,secret); free(msg);
    }
    int ok=truth_check(model,path,1,1,&hash,sig,secret!=NULL); free(hash); return ok;
}
static void sign_identity(LmbModelIdentity *id,const uint8_t secret[64]) {
    size_t n=0; uint8_t *msg=lmb_model_id_msg(id->model,id->root,&n); assert(msg);
    lmb_sign(id->sig,msg,n,secret); id->has_sig=1; free(msg);
}
#ifdef _WIN32
#include "tracker_history_fixture_windows.h"
#else
static int remove_test_file(const char *path,const struct stat *st,int kind,struct FTW *walk) {
    (void)st; (void)walk; return kind==FTW_DP ? rmdir(path) : unlink(path);
}
#endif
#ifdef _WIN32
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    char directory[512];wchar_t wide_directory[512],temp[400];
    if (argc==4 && !wcscmp(argv[1],L"--race")) {
        assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[2],-1,directory,sizeof directory,NULL,NULL));
        assert(snprintf(g_bindings_path,sizeof g_bindings_path,"%s/bindings",directory)<(int)sizeof g_bindings_path);
        unsigned value=(unsigned)wcstoul(argv[3],NULL,10);
        LmbModelIdentity id=identity("racing-root",value);return claim_model(&id,value,1) ? 0 : 1;
    }
    assert(argc==1 && GetTempPathW(400,temp));
    assert(swprintf(wide_directory,512,L"%lstracker-history-%lu-%llu-\x00e8",temp,GetCurrentProcessId(),GetTickCount64())>0);
    assert(CreateDirectoryW(wide_directory,NULL));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_directory,-1,directory,sizeof directory,NULL,NULL));
#else
int main(void) {
    char directory[]="/tmp/lmb-tracker-history.XXXXXX"; assert(mkdtemp(directory));
#endif
    assert(snprintf(g_bindings_path,sizeof g_bindings_path,"%s/bindings",directory)<(int)sizeof g_bindings_path);
    assert(!bindings_load());
    assert(!segment_generation_init());uint64_t generation=g_segment_route_generation;
    assert(!segment_generation_init() && g_segment_route_generation>generation);
    LmbModelIdentity first=identity("checkpoint-000",3);
    assert(!claim_model(&first,3,0)); /* unknown full root cannot start from a partial inventory */
    for (unsigned i=0;i<MAX_MODELS+12;i++) {
        char name[64]; snprintf(name,sizeof name,"checkpoint-%03u",i);
        LmbModelIdentity id=identity(name,3); assert(claim_model(&id,3,1));
    }
    GModel *model=NULL;
    assert(!model_lookup(first.model,&model) && !memcmp(model->id.root,first.root,32));
    assert(claim_model(&first,3,0)); /* partial donor after cache eviction */
    LmbModelIdentity wrong=identity(first.model,4); assert(!claim_model(&wrong,4,1));
    LmbSegAdvert advert={0}; snprintf(advert.model,sizeof advert.model,"%s",first.model);
    memcpy(advert.model_root,wrong.root,32); assert(!segment_model_root_ok(&advert));
    memcpy(advert.model_root,first.root,32); assert(segment_model_root_ok(&advert));

    for (unsigned i=0;i<MAX_FILES+4;i++) {
        char path[64]; snprintf(path,sizeof path,"weight-%04u",i);
        assert(claim_truth("truth-many",path,7,NULL));
    }
    GTruth *truth=NULL;
    assert(!truth_lookup("truth-many","weight-0000",&truth) && truth->hash[0]==7);
    assert(!claim_truth("truth-many","weight-0000",8,NULL));
    assert(claim_truth("truth-many","weight-0000",7,NULL));
    forget_caches();
    assert(!truth_lookup("","weight-0000",&truth) && !strcmp(truth->model,"truth-many"));
    assert(!model_lookup(first.model,&model) && !memcmp(model->id.root,first.root,32));

    /* Bootstrap a legacy flat file larger than the cache, then admit another
     * identity. Neither a restart nor eviction reopens the first name. */
    FILE *fp=tracker_state_open(g_bindings_path,1,1);assert(fp);
    uint8_t pk[32]={9},other[32]={10}; char hex[65]; lmb_hex(hex,pk,32);
    for (unsigned i=0;i<MAX_BINDINGS+4;i++) fprintf(fp,"E\t%s\tpeer-%04u\n",hex,i);
    assert(!tracker_state_flush(fp));tracker_state_close(fp);
    assert(!bindings_load() && g_nbindings==MAX_BINDINGS);
    assert(!binding_check_or_add("peer-new",1,pk));
    assert(binding_check_or_add("peer-0000",1,other));
    assert(!binding_check_or_add("peer-0000",1,pk));
    forget_caches(); assert(!bindings_load());
    assert(binding_check_or_add("peer-0000",1,other));

    /* Current trust-set order is recomputed, never read as a persisted rank. */
    uint8_t seed1[32]={1},seed2[32]={2},pk1[32],pk2[32],sk1[64],sk2[64];
    lmb_sign_keypair(pk1,sk1,seed1); lmb_sign_keypair(pk2,sk2,seed2);
    lmb_hex(hex,pk1,32); assert(!lmb_trust_add_hex(&g_trust,hex));
    LmbModelIdentity signed_model=identity("signed-history",11); sign_identity(&signed_model,sk1);
    assert(claim_model(&signed_model,11,1)); assert(claim_truth("signed-history","weights",11,sk1));
    lmb_hex(hex,pk2,32); assert(!lmb_trust_add_hex(&g_trust,hex));
    sign_identity(&signed_model,sk2); assert(claim_model(&signed_model,11,0));
    assert(claim_truth("signed-history","weights",11,sk2));
    forget_caches(); memset(&g_trust,0,sizeof g_trust); assert(!lmb_trust_add_hex(&g_trust,hex));
    assert(!model_lookup(signed_model.model,&model) && model->sig_rank==0);
    assert(!truth_lookup(signed_model.model,"weights",&truth) && truth->sig_rank==0);
    sign_identity(&signed_model,sk1); assert(!claim_model(&signed_model,11,0));
    assert(!claim_truth("signed-history","weights",11,sk1));
    memset(&g_trust,0,sizeof g_trust); forget_caches();
    signed_model.has_sig=0; assert(!claim_model(&signed_model,11,0));

    /* Two first registrants cannot publish different roots under one name. */
#ifdef _WIN32
    history_fixture_race(directory);
#else
    pid_t children[2];
    for (unsigned i=0;i<2;i++) {
        children[i]=fork(); assert(children[i]>=0);
        if (!children[i]) { LmbModelIdentity id=identity("racing-root",i+20); _exit(claim_model(&id,i+20,1) ? 0 : 1); }
    }
    int accepted=0;
    for (unsigned i=0;i<2;i++) { int status; assert(waitpid(children[i],&status,0)==children[i]);
        assert(WIFEXITED(status)); accepted+=WEXITSTATUS(status)==0; }
    assert(accepted==1);
#endif

    /* Corruption must not become an absent record/new first claim. */
    TrackerHistory history; assert(!tracker_history_begin(g_bindings_path,&history));
    char key[80]; tracker_history_key("model",first.model,"",key);
#ifdef _WIN32
    history_fixture_corrupt(&history,key,0);
#else
    int fd=lmb_journal_open(history.dir,key,O_WRONLY); assert(fd>=0);
    assert(pwrite(fd,"X",1,8)==1); close(fd);
#endif
    tracker_history_end(&history);
    forget_caches(); assert(model_lookup(first.model,&model)<0); assert(!claim_model(&first,3,1));
    assert(!segment_model_root_ok(&advert));
    assert(!tracker_history_begin(g_bindings_path,&history));
    tracker_history_key("truth","truth-many","weight-0000",key);
#ifdef _WIN32
    history_fixture_corrupt(&history,key,1);
#else
    fd=lmb_journal_open(history.dir,key,O_WRONLY); assert(fd>=0);
    assert(ftruncate(fd,12)==0); close(fd);
#endif
    tracker_history_end(&history);
    assert(truth_lookup("truth-many","weight-0000",&truth)<0);
    assert(!claim_truth("truth-many","weight-0000",7,NULL));
    forget_caches();
#ifdef _WIN32
    history_fixture_remove(wide_directory);
#else
    assert(!nftw(directory,remove_test_file,16,FTW_DEPTH|FTW_PHYS));
#endif
    puts("TRACKER HISTORY: PASS (cache overflow, legacy bindings, restart, exact roots/hashes, signature rotation, racing publication, corruption refusal)");
    return 0;
}
