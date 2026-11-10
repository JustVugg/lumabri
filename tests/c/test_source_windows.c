/* Production Windows tracker + checkpoint publisher; no fake file server. */
#include "lumabri_secure.h"
#include "src/runtime/lumabri_source_file.h"
#include "src/runtime/lumabri_source_match.h"
#include "src/runtime/lumabri_signed_checkpoint.h"
#include "src/runtime/lumabri_owned_process_windows.h"
#include <assert.h>
#include <pthread.h>
#include <winioctl.h>

static char folder[1500],root[1600],key[1600],tracker[64],source[64],donor[64];
static char maintainer_binary[6144],tracker_binary[6144],public_key[65];
static uint8_t *weights;static size_t weight_bytes;
static LmbOwnedProcess children[8];static unsigned count;
static HANDLE logs[8];
static void join(char *out,size_t cap,const char *a,const char *b) {
    int n=snprintf(out,cap,"%s/%s",a,b);assert(n>0 && (size_t)n<cap);
}
static void write_file(const char *path,const void *bytes,size_t size) {
    int fd=lmb_source_open(path,O_WRONLY|O_CREAT|O_TRUNC);assert(fd>=0);
    assert(lmb_source_pwrite(fd,bytes,size,0)==(ptrdiff_t)size);
    assert(!lmb_source_sync(fd) && !lmb_source_close(fd));
}
static void delete_fixture_file(const wchar_t *path) {
    /* Process handles are closed before cleanup, but Windows can retain a
     * transient file lock during teardown or scanning. Retry only lock/access
     * errors, with a deadline; permanent leaks still fail with the real error. */
    uint64_t until=GetTickCount64()+5000;
    for (;;) {
        if(DeleteFileW(path))return;
        DWORD error=GetLastError();
        if((error!=ERROR_SHARING_VIOLATION && error!=ERROR_LOCK_VIOLATION && error!=ERROR_ACCESS_DENIED) ||
            GetTickCount64()>=until) {
            fwprintf(stderr,L"Cannot delete owned fixture %ls: Windows error %lu\n",path,(unsigned long)error);
            abort();
        }
        Sleep(25);
    }
}
static void remove_fixture(const wchar_t *directory) {
    wchar_t pattern[2000];assert(swprintf(pattern,2000,L"%ls\\*",directory)>0);
    WIN32_FIND_DATAW e;HANDLE scan=FindFirstFileW(pattern,&e);assert(scan!=INVALID_HANDLE_VALUE);
    do {
        if (!wcscmp(e.cFileName,L".") || !wcscmp(e.cFileName,L"..")) continue;
        assert(!(e.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT));
        wchar_t child[2200];assert(swprintf(child,2200,L"%ls\\%ls",directory,e.cFileName)>0);
        if (e.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) remove_fixture(child);
        else delete_fixture_file(child);
    } while (FindNextFileW(scan,&e));
    FindClose(scan);assert(RemoveDirectoryW(directory));
}
static void choose_address(char out[64],char port[16]) {
    LmbSocket listener=lmb_listen(0);assert(listener!=LMB_INVALID_SOCKET);
    struct sockaddr_in bound;LmbSocketLength n=sizeof bound;
    assert(!getsockname(listener,(struct sockaddr *)&bound,&n));
    snprintf(port,16,"%u",ntohs(bound.sin_port));snprintf(out,64,"127.0.0.1:%s",port);lmb_close(listener);
}
static unsigned launch(const char *binary,const char *const *args,size_t n) {
    assert(count<8);unsigned i=count++;char path[1800];
    snprintf(path,sizeof path,"%s/child-%u.log",folder,i);wchar_t *wide=lmb_process_wide(path);assert(wide);
    logs[i]=CreateFileW(wide,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);free(wide);
    assert(logs[i]!=INVALID_HANDLE_VALUE);
    LmbProcessLaunch spec={.binary=binary,.args=args,.nargs=n,.output=logs[i],.error=logs[i]};
    assert(!lmb_owned_process_start(&children[i],&spec));return i;
}
static void stop(unsigned i) {
    assert(!lmb_owned_process_stop(&children[i],0,5000) && !lmb_owned_process_close(&children[i]));
    CloseHandle(logs[i]);logs[i]=NULL;
}
static void ready(const char *address,unsigned child) {
    uint64_t until=GetTickCount64()+15000;
    for (;;) {
        LmbSocket fd=lmb_connect_ms_io(address,200,1000);
        if (fd!=LMB_INVALID_SOCKET) {int ok=!lmb_auth(fd);lmb_close(fd);if (ok) return;}
        assert(GetTickCount64()<until && lmb_owned_process_alive(&children[child])==1);Sleep(20);
    }
}
static void read_exact(const char *address,const char *rel,uint64_t offset,const void *expected,uint32_t bytes) {
    LmbSocket fd=lmb_connect_ms_io(address,1000,3000);assert(fd!=LMB_INVALID_SOCKET && !lmb_auth(fd));
    LmbBuf body={0};LmbMsg reply={0};
    assert(!lmb_buf_str(&body,rel) && !lmb_buf_u64(&body,offset) && !lmb_buf_u32(&body,bytes) &&
        !lmb_send(fd,LMB_READ,body.p,(uint32_t)body.len,NULL,0) && !lmb_recv(fd,&reply));
    assert(reply.op==LMB_READ_R && reply.pay_len==bytes && !memcmp(reply.pay,expected,bytes));
    free(body.p);lmb_msg_free(&reply);lmb_close(fd);
}
static void *parallel_reads(void *arg) {
    uintptr_t index=(uintptr_t)arg;
    for (unsigned i=0;i<30;i++) {
        uint64_t offset=(index*197+i*65537)%(weight_bytes-4096);
        read_exact(source,"nested/weights.bin",offset,weights+offset,4096);
    }
    return NULL;
}
static LmbModelIdentity identity(const char *model,unsigned child) {
    LmbModelIdentity id;uint64_t until=GetTickCount64()+15000;
    while (lmb_model_identity_get(tracker,model,&id)) {
        assert(GetTickCount64()<until && lmb_owned_process_alive(&children[child])==1);Sleep(30);
    }
    assert(id.has_sig);size_t n;uint8_t *message=lmb_model_id_msg(model,id.root,&n);assert(message);
    assert(!lmb_sign_verify(id.sig,message,n,g_sec_pk));free(message);return id;
}
int wmain(int argc,wchar_t **argv) {
    assert(argc==3);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[1],-1,maintainer_binary,sizeof maintainer_binary,NULL,NULL));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[2],-1,tracker_binary,sizeof tracker_binary,NULL,NULL));
    wchar_t temp[400];char base[1200];assert(GetTempPathW(400,temp));
    assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,temp,-1,base,sizeof base,NULL,NULL));
    assert(snprintf(folder,sizeof folder,"%ssource-\xc3\xa8\xce\xa9-%lu-%llu",base,GetCurrentProcessId(),GetTickCount64())<(int)sizeof folder);
    assert(!lmb_private_mkdir(folder));
    wchar_t *profile=lmb_process_wide(folder);assert(profile && SetEnvironmentVariableW(L"LOCALAPPDATA",profile));free(profile);
    join(root,sizeof root,folder,"checkpoint");assert(!lmb_source_mkdir(root));
    join(key,sizeof key,folder,"key");char known[1600];join(known,sizeof known,folder,"known");
    assert(!_putenv_s("LUMABRI_PEER_KEY",key) && !_putenv_s("LUMABRI_KNOWN_HOSTS",known));
    assert(!_putenv_s("LUMABRI_TOKEN","source-native-test") && !_putenv_s("LUMABRI_ENCRYPT","1") && !lmb_secure_init());
    lmb_hex(public_key,g_sec_pk,32);
    assert(lmb_source_match("model-[0-9]*.bin","model-2/a.bin"));
    assert(lmb_source_match("[[:alpha:]][!0-9]?","Ax2"));
    assert(lmb_source_match("nested/*","nested/\xc3\xa8") && !lmb_source_match("*.BIN","x.bin"));
    assert(lmb_source_match("a\\*b","a*b") && !lmb_source_match("x*?","x"));
    char path[1800];join(path,sizeof path,root,"nested");assert(!lmb_source_mkdir(path));
    join(path,sizeof path,root,"nested/weights.bin");weight_bytes=2*LMB_HASH_CHUNK+37;weights=malloc(weight_bytes);assert(weights);
    for (size_t i=0;i<weight_bytes;i++) weights[i]=(uint8_t)(i*17+i/257);
    write_file(path,weights,weight_bytes);
    const char config[]="{\"model_type\":\"olmoe\"}";
    join(path,sizeof path,root,"config.json");write_file(path,config,sizeof config-1);
    join(path,sizeof path,root,".coli_usage");write_file(path,"private",7);
    /* A sparse positioned-I/O probe checks offsets beyond the CRT's 32-bit
     * limit without allocating or hashing a multi-gigabyte fixture. */
    join(path,sizeof path,folder,"sparse.bin");int sparse=lmb_source_open(path,O_RDWR|O_CREAT|O_EXCL);assert(sparse>=0);
    HANDLE sparse_handle=(HANDLE)_get_osfhandle(sparse);DWORD done;
    assert(DeviceIoControl(sparse_handle,FSCTL_SET_SPARSE,NULL,0,NULL,0,&done,NULL));
    const uint64_t at=UINT64_C(5)<<30;char readback[8];
    assert(lmb_source_pwrite(sparse,"offset!",7,at)==7 && lmb_source_pread(sparse,readback,7,at)==7 && !memcmp(readback,"offset!",7));
    LmbSourceStat st;assert(!lmb_source_fstat(sparse,&st) && st.size==at+7);
    assert(!lmb_source_close(sparse) && !lmb_source_unlink(path));
    char tp[16],sp[16],dp[16];choose_address(tracker,tp);choose_address(source,sp);choose_address(donor,dp);
    const char *ta[]={"--port",tp,"--pubkey",public_key};unsigned t=launch(tracker_binary,ta,4);ready(tracker,t);
    const char *sa[]={"--root",root,"--port",sp,"--tracker",tracker,"--name","native-source",
        "--model-name","native-source-fixture","--advertise",source,"--key",key};
    unsigned origin=launch(maintainer_binary,sa,14);ready(source,origin);
    LmbModelIdentity id=identity("native-source-fixture",origin);LmbSignedCheckpoint manifest;
    assert(!lmb_signed_checkpoint_fetch(tracker,id.model,g_sec_pk,id.root,weight_bytes+sizeof config-1,5000,&manifest));
    assert(manifest.count==2);lmb_signed_checkpoint_free(&manifest);
    assert(!_putenv_s("LUMABRI_TOKEN","wrong-key"));
    LmbSocket denied=lmb_connect_ms_io(source,1000,1000);assert(denied!=LMB_INVALID_SOCKET && lmb_auth(denied));lmb_close(denied);
    assert(!_putenv_s("LUMABRI_TOKEN","source-native-test"));
    pthread_t readers[4];for (uintptr_t i=0;i<4;i++) assert(!pthread_create(&readers[i],NULL,parallel_reads,(void *)i));
    for (unsigned i=0;i<4;i++) assert(!pthread_join(readers[i],NULL));
    stop(origin);origin=launch(maintainer_binary,sa,14);ready(source,origin);
    LmbModelIdentity same=identity(id.model,origin);assert(!memcmp(id.root,same.root,32));
    char copy[1600];join(copy,sizeof copy,folder,"copy");
    const char *da[]={"--root",copy,"--port",dp,"--tracker",tracker,"--name","native-copy",
        "--model-name",id.model,"--advertise",donor,"--pubkey",public_key,"--donate","1"};
    unsigned replica=launch(maintainer_binary,da,16);ready(donor,replica);stop(origin);
    read_exact(donor,"nested/weights.bin",17,weights+17,65536);
    stop(replica);replica=launch(maintainer_binary,da,16);ready(donor,replica);
    read_exact(donor,"config.json",0,config,sizeof config-1);stop(replica);stop(t);
    /* A changed checkpoint cannot replace a tracker identity retained across
     * restart. A new model identity must still carry the new byte hashes. */
    join(path,sizeof path,root,"nested/weights.bin");weights[0]^=1;write_file(path,weights,weight_bytes);
    choose_address(tracker,tp);t=launch(tracker_binary,ta,4);ready(tracker,t);
    origin=launch(maintainer_binary,sa,14);ready(source,origin);
    LmbModelIdentity retained=identity(id.model,origin);assert(!memcmp(id.root,retained.root,32));
    stop(origin);sa[9]="native-source-fixture-v2";origin=launch(maintainer_binary,sa,14);ready(source,origin);
    LmbModelIdentity changed=identity(sa[9],origin);assert(memcmp(id.root,changed.root,32));
    assert(!lmb_signed_checkpoint_fetch(tracker,changed.model,g_sec_pk,changed.root,weight_bytes+sizeof config-1,5000,&manifest));
    uint8_t hash[32];lmb_sha256(weights,LMB_HASH_CHUNK,hash);int checked=0;
    for (unsigned i=0;i<manifest.count;i++) if (!strcmp(manifest.items[i].path,"nested/weights.bin")) {
        assert(!memcmp(manifest.items[i].hashes,hash,32));checked=1;
    }
    assert(checked);lmb_signed_checkpoint_free(&manifest);
    read_exact(source,"nested/weights.bin",0,weights,4096);
    stop(origin);stop(t);free(weights);
    profile=lmb_process_wide(folder);assert(profile);remove_fixture(profile);free(profile);
    puts("WINDOWS CHECKPOINT SOURCE: PASS (production signed tracker/source, encrypted transfer, Unicode, parallel ranges, 64-bit offsets, source restart, replica survives origin, same-size mutation)");return 0;
}
