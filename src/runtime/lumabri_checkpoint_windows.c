/* Sparse files expose only verified metadata to the planner. Engine weight
 * reads go through the sealed input boundary, directly from signed chunks.
 * This unit must not be compiled with the engine input/allocator overrides. */
#include "lumabri_checkpoint_windows.h"
#include "lumabri_signed_checkpoint.h"
#include "lumabri_private_file_windows.h"
#include "lumabri_engine_io_windows.h"
#include <winioctl.h>

#define INPUT_CACHE 8u
#define INPUT_METADATA_BYTES (64u<<20)
#define INPUT_FILE_METADATA_BYTES (16u<<20)
typedef struct {
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION identity;
    char *path;
} InputFile;
typedef struct { uint32_t file,chunk,size;uint8_t *data;uint64_t used; } InputChunk;
typedef struct {
    LmbSignedCheckpoint manifest;
    InputFile *files;
    uint32_t count;
    HANDLE directory;
    char path[4096],peer[256];
    LmbSocket socket;
    SRWLOCK lock;
    InputChunk cache[INPUT_CACHE];
    uint64_t clock,fetches,hits,bytes;
    int sealed;
} NativeInput;
static NativeInput *active_input;

/* Win32 names have more aliases than POSIX names. Reject reserved devices,
 * ADS, trailing dots/spaces and case aliases before creating any placeholder. */
static int input_path_ok(const char *path) {
    if (!lmb_rel_ok(path) || path[strlen(path)-1]=='/') return 0;
    const char *p=path;
    while (*p) {
        const char *start=p;
        while (*p && *p!='/') {if (strchr(":<>\"|?*",*p)) return 0;p++;}
        size_t n=(size_t)(p-start),stem=0;
        if (!n || start[n-1]=='.' || start[n-1]==' ') return 0;
        while (stem<n && start[stem]!='.') stem++;
        if ((stem==3 && (!_strnicmp(start,"CON",3) || !_strnicmp(start,"PRN",3) ||
                         !_strnicmp(start,"AUX",3) || !_strnicmp(start,"NUL",3))) ||
            (stem==4 && (!_strnicmp(start,"COM",3) || !_strnicmp(start,"LPT",3)) &&
                        start[3]>='0' && start[3]<='9')) return 0;
        if (*p) p++;
    }
    return 1;
}

static void input_disconnect(NativeInput *s) {
    if (s->socket!=LMB_INVALID_SOCKET) lmb_close(s->socket);
    s->socket=LMB_INVALID_SOCKET;s->peer[0]=0;
}
static InputChunk *input_chunk(NativeInput *s,uint32_t file,uint32_t index) {
    LmbModelItem *item=&s->manifest.items[file];
    if (index>=item->nh) return NULL;
    InputChunk *slot=&s->cache[0];
    for (unsigned i=0;i<INPUT_CACHE;i++) {
        InputChunk *c=&s->cache[i];
        if (c->data && c->file==file && c->chunk==index) {
            c->used=++s->clock;s->hits++;return c;
        }
        if (!c->data || (slot->data && c->used<slot->used)) slot=c;
    }
    uint64_t at=(uint64_t)index*LMB_HASH_CHUNK;
    uint32_t want=(uint32_t)(item->size-at>LMB_HASH_CHUNK ? LMB_HASH_CHUNK : item->size-at);
    LmbCheckpointSources *sources=&s->manifest.sources[file];
    for (unsigned p=0;p<sources->count;p++) {
        const char *peer=sources->address[p];
        /* A pooled connection can have closed between reads; retry once. */
        for (unsigned retry=0;retry<2;retry++) {
            if (s->socket!=LMB_INVALID_SOCKET && strcmp(s->peer,peer)) input_disconnect(s);
            if (s->socket==LMB_INVALID_SOCKET) {
                s->socket=lmb_connect_ms_io(peer,1500,15000);
                if (s->socket!=LMB_INVALID_SOCKET) lmb_set_io_timeout(s->socket,15000);
                uint64_t previous=lmb_read_deadline_ms,deadline=lmb_io_monotonic_ms()+15000;
                if (!previous || deadline<previous) lmb_read_deadline_ms=deadline;
                int bad=s->socket==LMB_INVALID_SOCKET || lmb_auth(s->socket);
                lmb_read_deadline_ms=previous;
                if (bad) {input_disconnect(s);break;}
                strcpy(s->peer,peer);
            }
            LmbBuf request={0};LmbMsg reply={0};uint8_t hash[32];
            int bad=lmb_buf_str(&request,item->path) || lmb_buf_u64(&request,at) ||
                lmb_buf_u32(&request,want) ||
                lmb_send(s->socket,LMB_READ,request.p,(uint32_t)request.len,NULL,0) ||
                lmb_recv_bounded(s->socket,&reply,LMB_HASH_CHUNK+4096,15000) ||
                reply.op!=LMB_READ_R || reply.body_len || reply.pay_len!=want;
            free(request.p);
            if (!bad) {lmb_sha256(reply.pay,want,hash);bad=memcmp(hash,item->hashes+(size_t)index*32,32)!=0;}
            if (!bad) {
                free(slot->data);slot->data=lmb_msg_take_pay(&reply);
                slot->file=file;slot->chunk=index;slot->size=want;slot->used=++s->clock;
                s->fetches++;s->bytes+=want;lmb_msg_free(&reply);return slot;
            }
            lmb_msg_free(&reply);input_disconnect(s);
        }
    }
    errno=EIO;return NULL;
}

static int input_read_at(NativeInput *s,uint32_t file,void *buffer,size_t size,uint64_t at) {
    if (file>=s->count || (size && (at>s->manifest.items[file].size ||
        size>s->manifest.items[file].size-at))) return -1;
    uint8_t *out=buffer;
    while (size) {
        InputChunk *chunk=input_chunk(s,file,(uint32_t)(at/LMB_HASH_CHUNK));
        if (!chunk) return -1;
        size_t offset=(size_t)(at%LMB_HASH_CHUNK),take=chunk->size-offset;
        if (take>size) take=size;
        memcpy(out,chunk->data+offset,take);at+=take;out+=take;size-=take;
    }
    return 0;
}

static int input_engine_read(void *context,HANDLE file,void *buffer,size_t size,uint64_t at) {
    NativeInput *s=context;BY_HANDLE_FILE_INFORMATION identity;
    if (!s || !GetFileInformationByHandle(file,&identity) || identity.nNumberOfLinks!=1 ||
        (identity.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return -1;
    AcquireSRWLockExclusive(&s->lock);int result=-1;
    for (uint32_t i=0;!s->sealed && i<s->count;i++) {
        BY_HANDLE_FILE_INFORMATION *own=&s->files[i].identity;
        if (identity.dwVolumeSerialNumber==own->dwVolumeSerialNumber &&
            identity.nFileIndexHigh==own->nFileIndexHigh && identity.nFileIndexLow==own->nFileIndexLow) {
            uint64_t length=((uint64_t)identity.nFileSizeHigh<<32)|identity.nFileSizeLow;
            if (length==s->manifest.items[i].size) result=input_read_at(s,i,buffer,size,at);
            break;
        }
    }
    ReleaseSRWLockExclusive(&s->lock);return result;
}

static void input_seal(void *context) {
    NativeInput *s=context;if (!s || s->sealed) return;
    s->sealed=1;input_disconnect(s);
    for (unsigned i=0;i<INPUT_CACHE;i++) {free(s->cache[i].data);s->cache[i].data=NULL;}
    lmb_signed_checkpoint_free(&s->manifest);
    if (s->fetches) fprintf(stderr,"[checkpoint-input] %llu verified chunks, %llu cache hits, %llu bytes; preparation buffers released\n",
        (unsigned long long)s->fetches,(unsigned long long)s->hits,(unsigned long long)s->bytes);
}

static void input_delete(const char *path,int directory) {
    wchar_t wide[4096];
    if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096)) {
        if (directory) (void)RemoveDirectoryW(wide);else (void)DeleteFileW(wide);
    }
}
static void input_destroy(NativeInput *s) {
    if (!s) return;
    input_seal(s);
    if (s->files) for (uint32_t i=0;i<s->count;i++) {
        if (s->files[i].handle && s->files[i].handle!=INVALID_HANDLE_VALUE) CloseHandle(s->files[i].handle);
        if (s->files[i].path) {input_delete(s->files[i].path,0);free(s->files[i].path);}
    }
    free(s->files);
    if (s->directory && s->directory!=INVALID_HANDLE_VALUE) CloseHandle(s->directory);
    /* Nested metadata folders may remain empty; the keeper owns the private
     * allocation directory and removes it only after proving worker exit. */
    free(s);
}
static void input_exit(void) {input_destroy(active_input);active_input=NULL;}

static int input_create_file(NativeInput *s,uint32_t i) {
    char path[4096];const char *relative=s->manifest.items[i].path;
    if (snprintf(path,sizeof path,"%s/%s",s->path,relative)>=(int)sizeof path) return -1;
    for (char *p=path+strlen(s->path)+1;*p;p++) if (*p=='/') {
        *p=0;int bad=lmb_private_mkdir(path);*p='/';if (bad) return -1;
    }
    InputFile *file=&s->files[i];int created=0;
    file->handle=lmb_private_open(path,1,1,&created);
    if (file->handle==INVALID_HANDLE_VALUE) return -1;
    if (!created) {CloseHandle(file->handle);file->handle=INVALID_HANDLE_VALUE;errno=EEXIST;return -1;}
    file->path=strdup(path);if (!file->path) return -1;
    LARGE_INTEGER length;length.QuadPart=(LONGLONG)s->manifest.items[i].size;DWORD ignored;
    if (!DeviceIoControl(file->handle,FSCTL_SET_SPARSE,NULL,0,NULL,0,&ignored,NULL) ||
        !SetFilePointerEx(file->handle,length,NULL,FILE_BEGIN) || !SetEndOfFile(file->handle) ||
        !GetFileInformationByHandle(file->handle,&file->identity)) return -1;
    return 0;
}
static int input_metadata(NativeInput *s,uint32_t i,uint64_t *total) {
    const LmbModelItem *item=&s->manifest.items[i];size_t length=strlen(item->path);
    int tensor=length>=12 && !strcmp(item->path+length-12,".safetensors");
    uint64_t size=item->size;
    if (tensor) {
        uint8_t header[8];if (size<8 || input_read_at(s,i,header,8,0)) return -1;
        uint64_t n=0;for (unsigned b=0;b<8;b++) n|=(uint64_t)header[b]<<(b*8);
        if (!n || n>size-8 || n>INPUT_FILE_METADATA_BYTES-8) return -1;
        size=n+8;
    }
    if (size>INPUT_FILE_METADATA_BYTES || size>INPUT_METADATA_BYTES-*total) return -1;
    *total+=size;
    LARGE_INTEGER zero={0};if (!SetFilePointerEx(s->files[i].handle,zero,NULL,FILE_BEGIN)) return -1;
    uint64_t at=0;
    while (at<size) {
        InputChunk *chunk=input_chunk(s,i,(uint32_t)(at/LMB_HASH_CHUNK));
        if (!chunk) return -1;
        DWORD take=(DWORD)(size-at>chunk->size ? chunk->size : size-at),written=0;
        if (!WriteFile(s->files[i].handle,chunk->data,take,&written,NULL) || written!=take) return -1;
        at+=take;
    }
    return 0;
}

int lmb_checkpoint_windows_prepare(const char *directory,const char *tracker,
    const char *model,const uint8_t signer[32],const uint8_t root[32],uint64_t bytes) {
    if (active_input || !directory || strlen(directory)>=4096) return -1;
    NativeInput *s=calloc(1,sizeof *s);if (!s) return -1;
    const char *stage="signed manifest";
    s->socket=LMB_INVALID_SOCKET;strcpy(s->path,directory);InitializeSRWLock(&s->lock);
    if (lmb_signed_checkpoint_fetch(tracker,model,signer,root,bytes,30000,&s->manifest)) goto fail;
    s->count=s->manifest.count;s->files=calloc(s->count,sizeof *s->files);if (!s->files) goto fail;
    stage="checkpoint paths";
    for (uint32_t i=0;i<s->count;i++) {
        if (!input_path_ok(s->manifest.items[i].path)) goto fail;
        for (uint32_t j=0;j<i;j++) if (!_stricmp(s->manifest.items[i].path,s->manifest.items[j].path)) goto fail;
    }
    stage="private preparation directory";
    if (lmb_private_mkdir(directory)) goto fail;
    LmbPrivatePath path;
    if (lmb_private_path(directory,&path)) goto fail;
    s->directory=CreateFileW(path.path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    lmb_private_path_close(&path);if (s->directory==INVALID_HANDLE_VALUE) goto fail;
    uint64_t metadata=0;
    stage="verified metadata";
    for (uint32_t i=0;i<s->count;i++) if (input_create_file(s,i) || input_metadata(s,i,&metadata)) goto fail;
    stage="engine input boundary";
    if (lmb_engine_input_provider(s,input_engine_read,input_seal)) goto fail;
    active_input=s;atexit(input_exit);return 0;
fail:
    fprintf(stderr,"[checkpoint-input] preparation failed: %s\n",stage);
    input_destroy(s);return -1;
}
