/* Test-only signed source. Production native input is exercised without a
 * preinstalled checkpoint, using the same placement/hash/read wire codec. */
#include "lumabri_proto.h"
#include "lumabri_secure.h"
#include "lumabri_sha.h"
#include <assert.h>

typedef struct {
    LmbModelItem items[32];
    uint8_t *data[32],root[32],key[32],secret[64];
    char model[64],address[128];
    uint32_t count;
    uint64_t bytes;
    atomic_uint_fast64_t reads;
    int corrupt;
} CheckpointFixture;

static void checkpoint_fixture_root(CheckpointFixture *f) {
    assert(!lmb_model_root(f->model,f->items,f->count,f->root));
}
static void checkpoint_fixture_add(CheckpointFixture *f,const char *path,uint8_t *data,size_t size) {
    assert(f->count<32);uint32_t index=f->count++;
    LmbModelItem *item=&f->items[index];item->path=strdup(path);assert(item->path);
    item->size=size;item->nh=(uint32_t)(size/LMB_HASH_CHUNK+(size%LMB_HASH_CHUNK!=0));
    uint8_t *hashes=calloc(item->nh ? item->nh : 1,32);assert(hashes);item->hashes=hashes;
    for (uint32_t i=0;i<item->nh;i++) {
        size_t at=(size_t)i*LMB_HASH_CHUNK,n=size-at;if (n>LMB_HASH_CHUNK) n=LMB_HASH_CHUNK;
        lmb_sha256(data+at,n,hashes+(size_t)i*32);
    }
    f->data[index]=data;f->bytes+=size;checkpoint_fixture_root(f);
}
static void checkpoint_fixture_init(CheckpointFixture *f,const char *model) {
    memset(f,0,sizeof *f);strcpy(f->model,model);
    atomic_init(&f->reads,0);
    uint8_t seed[32];lmb_random(seed,32);lmb_sign_keypair(f->key,f->secret,seed);
}
static void checkpoint_fixture_load(CheckpointFixture *f,const char *directory) {
    wchar_t wide[4096],pattern[4100];
    assert(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,directory,-1,wide,4096));
    assert(swprintf(pattern,4100,L"%ls\\*",wide)>0);
    WIN32_FIND_DATAW found;HANDLE search=FindFirstFileW(pattern,&found);assert(search!=INVALID_HANDLE_VALUE);
    do {
        if (found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[4096];wchar_t source[8192];
        assert(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,found.cFileName,-1,path,sizeof path,NULL,NULL));
        assert(swprintf(source,8192,L"%ls\\%ls",wide,found.cFileName)>0);
        FILE *file=_wfopen(source,L"rb");assert(file);
        assert(!_fseeki64(file,0,SEEK_END));__int64 size=_ftelli64(file);
        assert(size>=0 && size<=32*1024*1024 && !_fseeki64(file,0,SEEK_SET));
        uint8_t *data=malloc(size ? (size_t)size : 1);assert(data);
        assert(fread(data,1,(size_t)size,file)==(size_t)size);fclose(file);
        checkpoint_fixture_add(f,path,data,(size_t)size);
    } while (FindNextFileW(search,&found));
    FindClose(search);assert(f->count);
}
static void checkpoint_fixture_free(CheckpointFixture *f) {
    for (uint32_t i=0;i<f->count;i++) {
        free((void *)f->items[i].path);free((void *)f->items[i].hashes);free(f->data[i]);
    }
}
static int checkpoint_fixture_reply(CheckpointFixture *f,LmbSocket fd,const LmbMsg *m) {
    if (m->op!=LMB_PLACEMENT && m->op!=LMB_HASHES && m->op!=LMB_READ) return 0;
    assert(f->count && !m->pay_len);
    LmbCur c={m->body,m->body_len,0};LmbBuf body={0};char model[64],path[LMB_PATH_MAX];
    if (m->op!=LMB_READ) assert(!lmb_cur_str(&c,model,sizeof model) && !strcmp(model,f->model));
    if (m->op==LMB_PLACEMENT) {
        assert(c.off==c.len && !lmb_buf_u32(&body,f->count));
        for (uint32_t i=0;i<f->count;i++) {
            assert(!lmb_buf_str(&body,f->items[i].path) && !lmb_buf_u64(&body,f->items[i].size) &&
                !lmb_buf_u16(&body,1) && !lmb_buf_str(&body,f->address));
        }
        assert(!lmb_send(fd,LMB_PLACEMENT_R,body.p,(uint32_t)body.len,NULL,0));
    } else {
        assert(!lmb_cur_str(&c,path,sizeof path));uint32_t i=0;
        while (i<f->count && strcmp(f->items[i].path,path)) i++;
        assert(i<f->count);LmbModelItem *item=&f->items[i];
        if (m->op==LMB_HASHES) {
            assert(c.off==c.len);
            size_t size=0;uint8_t sig[64];
            uint8_t *message=lmb_truth_msg(f->model,item->path,LMB_HASH_CHUNK,item->size,item->hashes,item->nh,&size);
            assert(message);lmb_sign(sig,message,size,f->secret);free(message);
            assert(!lmb_buf_str(&body,f->model) && !lmb_buf_u32(&body,LMB_HASH_CHUNK) &&
                !lmb_buf_u32(&body,item->nh) && !lmb_buf_u64(&body,item->size) && !lmb_buf_u32(&body,1) &&
                !lmb_buf_bytes(&body,sig,64));
            assert(!lmb_send(fd,LMB_HASHES_R,body.p,(uint32_t)body.len,item->hashes,item->nh*32));
        } else {
            uint64_t at;uint32_t count;
            assert(!lmb_cur_u64(&c,&at) && !lmb_cur_u32(&c,&count) && c.off==c.len &&
                at<=item->size && count<=item->size-at && count<=LMB_HASH_CHUNK);
            uint8_t *copy=malloc(count ? count : 1);assert(copy);memcpy(copy,f->data[i]+at,count);
            if (f->corrupt && count) copy[count/2]^=1;
            f->reads++;assert(!lmb_send(fd,LMB_READ_R,NULL,0,copy,count));free(copy);
        }
    }
    free(body.p);return 1;
}
