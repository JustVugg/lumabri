#include "checkpoint_fixture_windows.h"
#include "src/runtime/lumabri_checkpoint_windows.h"
#include "src/runtime/lumabri_engine_io_windows.h"
#include "src/runtime/lumabri_engine_memory.h"
#include "src/runtime/lumabri_private_file_windows.h"
#include <pthread.h>

typedef struct {CheckpointFixture files;LmbSocket listener;pthread_t thread;pthread_mutex_t lock;atomic_int stop;} Source;
static char cleanup_folder[4096];
static void cleanup(void) {
    const char *files[]={"checkpoint/weights.safetensors","checkpoint/config.json","key","known"};
    for (unsigned i=0;i<sizeof files/sizeof *files;i++) {
        char path[8192];wchar_t wide[8192];
        if (snprintf(path,sizeof path,"%s/%s",cleanup_folder,files[i])<(int)sizeof path &&
            MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,8192)) (void)DeleteFileW(wide);
    }
    char path[8192];wchar_t wide[8192];
    snprintf(path,sizeof path,"%s/checkpoint",cleanup_folder);
    if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,8192)) (void)RemoveDirectoryW(wide);
    if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,cleanup_folder,-1,wide,8192)) (void)RemoveDirectoryW(wide);
}
static void *serve(void *context) {
    Source *s=context;
    while (!atomic_load(&s->stop)) {
        if (lmb_socket_wait(s->listener,0,10)!=1) continue;
        LmbSocket fd=lmb_socket_accept(s->listener,NULL,NULL);assert(fd!=LMB_INVALID_SOCKET);
        assert(!lmb_socket_timeout(fd,1000) && !lmb_secure_server(fd));
        int auth=0;LmbMsg m={0};
        while (!atomic_load(&s->stop) && !lmb_recv_bounded(fd,&m,4096,1000)) {
            if (m.op==LMB_AUTH) {
                char token[64];LmbCur c={m.body,m.body_len,0};
                assert(!lmb_cur_str(&c,token,sizeof token) && !strcmp(token,"native-input-test"));
                auth=1;assert(!lmb_send(fd,LMB_OK,NULL,0,NULL,0));
            } else {
                pthread_mutex_lock(&s->lock);
                assert(auth && checkpoint_fixture_reply(&s->files,fd,&m));
                pthread_mutex_unlock(&s->lock);
            }
            lmb_msg_free(&m);
        }
        lmb_msg_free(&m);lmb_close(fd);
    }
    return NULL;
}
int main(int argc,char **argv) {
    fprintf(stderr,"checkpoint gate: start\n");fflush(stderr);
    int invalid=argc==2 && !strcmp(argv[1],"--invalid-handle");
    assert(argc==1 || invalid);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    wchar_t wide[1024];char temp[4096],folder[4096],identity[4096],known[4096],checkpoint[4096];
    assert(GetTempPathW(1024,wide));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,temp,sizeof temp,NULL,NULL));
    assert(snprintf(folder,sizeof folder,"%sinput \xc3\xa8\xce\xa9-%lu-%llu",temp,(unsigned long)GetCurrentProcessId(),
        (unsigned long long)GetTickCount64())<(int)sizeof folder);
    assert(!lmb_private_mkdir(folder));
    strcpy(cleanup_folder,folder);assert(!atexit(cleanup));
    assert(snprintf(identity,sizeof identity,"%s/key",folder)<(int)sizeof identity);
    assert(snprintf(known,sizeof known,"%s/known",folder)<(int)sizeof known);
    assert(snprintf(checkpoint,sizeof checkpoint,"%s/checkpoint",folder)<(int)sizeof checkpoint);
    assert(!_putenv_s("LUMABRI_PEER_KEY",identity) && !_putenv_s("LUMABRI_KNOWN_HOSTS",known));
    assert(!_putenv_s("LUMABRI_TOKEN","native-input-test") && !_putenv_s("LUMABRI_ENCRYPT","1"));
    assert(!lmb_secure_init());
    fprintf(stderr,"checkpoint gate: identity ready\n");fflush(stderr);
    Source source={0};atomic_init(&source.stop,0);pthread_mutex_init(&source.lock,NULL);
    CheckpointFixture *f=&source.files;
    checkpoint_fixture_init(f,"native-input");
    size_t size=LMB_HASH_CHUNK*10+31;uint8_t *data=malloc(size);assert(data);
    for (size_t i=0;i<size;i++) data[i]=(uint8_t)(i*17+i/257);
    memset(data,0,8);data[0]=16;memset(data+8,' ',16);
    checkpoint_fixture_add(f,"weights.safetensors",data,size);
    uint8_t *config=(uint8_t *)strdup("{\"model_type\":\"olmoe\"}");assert(config);
    checkpoint_fixture_add(f,"config.json",config,strlen((char *)config));
    source.listener=lmb_listen(0);assert(source.listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in bound;LmbSocketLength len=sizeof bound;
    assert(!getsockname(source.listener,(struct sockaddr *)&bound,&len));
    snprintf(f->address,sizeof f->address,"127.0.0.1:%u",ntohs(bound.sin_port));
    assert(!pthread_create(&source.thread,NULL,serve,&source));
    uint8_t changed[32];memcpy(changed,f->key,32);changed[0]^=1;
    assert(lmb_checkpoint_windows_prepare(checkpoint,f->address,f->model,changed,f->root,f->bytes));
    memcpy(changed,f->root,32);changed[0]^=1;
    assert(lmb_checkpoint_windows_prepare(checkpoint,f->address,f->model,f->key,changed,f->bytes));
    assert(lmb_checkpoint_windows_prepare(checkpoint,f->address,f->model,f->key,f->root,f->bytes+1));
    const char *bad_paths[]={"../escape","CON.json","weights.safetensors:other","folder./x","config.JSON"};
    const char *original=f->items[0].path;
    for (unsigned i=0;i<sizeof bad_paths/sizeof *bad_paths;i++) {
        pthread_mutex_lock(&source.lock);
        f->items[0].path=bad_paths[i];checkpoint_fixture_root(f);
        pthread_mutex_unlock(&source.lock);
        assert(lmb_checkpoint_windows_prepare(checkpoint,f->address,f->model,f->key,f->root,f->bytes));
    }
    pthread_mutex_lock(&source.lock);
    f->items[0].path=original;checkpoint_fixture_root(f);f->corrupt=1;
    pthread_mutex_unlock(&source.lock);
    assert(lmb_checkpoint_windows_prepare(checkpoint,f->address,f->model,f->key,f->root,f->bytes));
    pthread_mutex_lock(&source.lock);f->corrupt=0;pthread_mutex_unlock(&source.lock);
    uint64_t before=atomic_load(&f->reads);
    assert(!lmb_checkpoint_windows_prepare(checkpoint,f->address,f->model,f->key,f->root,f->bytes));
    assert(atomic_load(&f->reads)==before+2); /* metadata, not the full 10 MiB weight file */
    fprintf(stderr,"checkpoint gate: input ready\n");fflush(stderr);
    char path[4096];assert(snprintf(path,sizeof path,"%s/weights.safetensors",checkpoint)<(int)sizeof path);
    HANDLE file=lmb_private_open(path,0,0,NULL);assert(file!=INVALID_HANDLE_VALUE);
    uint8_t actual[1000];DWORD got;OVERLAPPED at={0};at.Offset=1024;
    assert(ReadFile(file,actual,sizeof actual,&got,&at) && got==sizeof actual);
    for (unsigned i=0;i<sizeof actual;i++) assert(!actual[i]); /* no weight payload persisted */
    for (unsigned i=0;i<13;i++) {
        at.Offset=(i%11)*LMB_HASH_CHUNK+(i%11==10 ? 0 : LMB_HASH_CHUNK-500);
        DWORD want=size-at.Offset<sizeof actual ? (DWORD)(size-at.Offset) : sizeof actual;
        assert(lmb_engine_ReadFile(file,actual,want,&got,&at) && got==want);
        assert(!memcmp(actual,data+at.Offset,want));
    }
    FILE *stream=fopen(path,"rb");assert(stream && !_fseeki64(stream,LMB_HASH_CHUNK-400,SEEK_SET));
    assert(lmb_engine_fread(actual,10,100,stream)==100 && !memcmp(actual,data+LMB_HASH_CHUNK-400,1000));
    fclose(stream);
    fprintf(stderr,"checkpoint gate: ranged reads ready\n");fflush(stderr);
    if (invalid) {
        HANDLE unknown=lmb_private_open(identity,0,0,NULL);assert(unknown!=INVALID_HANDLE_VALUE);
        assert(!lmb_engine_ReadFile(unknown,actual,1,&got,NULL) && !got);CloseHandle(unknown);
    }
    /* Give the seal real owned locked memory, then prove no source input
     * is requested by any post-seal file access. */
    assert(!_putenv_s("LUMABRI_RESIDENCY_POLICY","engine-locked"));
    assert(!_putenv_s("LUMABRI_RESIDENT_REQUIRED","1"));
    assert(SetProcessWorkingSetSize(GetCurrentProcess(),8u<<20,128u<<20));
    void *weight=lmb_engine_malloc(4096);assert(weight);
    extern void lmb_resident_adapter_prepared(void);lmb_resident_adapter_prepared();
    if (invalid) assert(lmb_engine_input_seal()); /* input failure permanently blocks READY */
    else {
        assert(!lmb_engine_input_seal());
        assert(!lmb_engine_ReadFile(file,actual,1,&got,&at) && !got && GetLastError()==ERROR_ACCESS_DENIED);
    }
    CloseHandle(file);lmb_engine_free(weight);
    fprintf(stderr,"checkpoint gate: joining source\n");fflush(stderr);
    atomic_store(&source.stop,1);pthread_join(source.thread,NULL);lmb_close(source.listener);
    pthread_mutex_destroy(&source.lock);
    assert(f->reads>=11);checkpoint_fixture_free(f);
    puts("NATIVE CHECKPOINT: PASS (signer/root/size fences, path aliases, corrupted chunks, sparse metadata, verified ranged reads and seal)");
    return 0;
}
