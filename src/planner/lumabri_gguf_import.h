/* Local metadata cache for raw GGUF catalog entries. Weights are hard-linked,
 * never copied/requantized. All published names stay inside the checkpoint
 * root. The normal signed-source flow subsequently verifies their bytes. */
#ifndef LMB_GGUF_IMPORT_H
#define LMB_GGUF_IMPORT_H
#include "lumabri_gguf_metadata.h"
#include "src/runtime/lumabri_source_file.h"
#include "src/runtime/lumabri_weight_container.h"
#include "lumabri_sha.h"
#include <stdatomic.h>

static inline int lmb_gguf_link(const char *source,const char *target) {
#ifdef _WIN32
    LmbPrivatePath a,b;if(lmb_private_path(source,&a))return -1;
    if(lmb_private_path(target,&b)){lmb_private_path_close(&a);return -1;}
    int rc=CreateHardLinkW(b.path,a.path,NULL)?0:lmb_private_error(GetLastError());
    lmb_private_path_close(&a);lmb_private_path_close(&b);return rc;
#else
    return linkat(AT_FDCWD,source,AT_FDCWD,target,AT_SYMLINK_FOLLOW);
#endif
}
static inline int lmb_gguf_import_temporary(const char *output,char *tmp,size_t capacity) {
    static atomic_uint sequence=0;
#ifdef _WIN32
    unsigned long pid=GetCurrentProcessId();
#else
    unsigned long pid=(unsigned long)getpid();
#endif
    for(unsigned i=0;i<64;i++) {
        int n=snprintf(tmp,capacity,"%s-%lu-%u.tmp",output,pid,atomic_fetch_add(&sequence,1));
        if(n<0 || (size_t)n>=capacity)return -1;
        if(!lmb_source_mkdir(tmp))return 0;
        if(errno!=EEXIST)return -1;
    }
    return -1;
}
static inline void lmb_gguf_import_rmdir(const char *path) {
#ifdef _WIN32
    LmbPrivatePath p;if(!lmb_private_path(path,&p)){RemoveDirectoryW(p.path);lmb_private_path_close(&p);}
#else
    (void)rmdir(path);
#endif
}
static inline int lmb_gguf_file_text(const char *path,const LmbGgufJson *j,int create) {
    int fd=lmb_source_open(path,create?O_WRONLY|O_CREAT|O_EXCL:O_RDONLY);
    if(fd<0)return -1;
    int bad=0;
    if(!create){LmbSourceStat st;if(lmb_source_fstat(fd,&st) || st.size!=j->length)bad=1;}
    size_t at=0;unsigned char data[8192];
    while(!bad && at<j->length) {
        size_t n=j->length-at;if(n>sizeof data)n=sizeof data;
        ptrdiff_t done=create?lmb_source_pwrite(fd,j->data+at,n,at):lmb_source_pread(fd,data,n,at);
        if(done<0 && errno==EINTR)continue;
        if(done<=0 || (!create && memcmp(data,j->data+at,(size_t)done)))bad=1;
        else at+=(size_t)done;
    }
    if(create && !bad && lmb_source_sync(fd))bad=1;
    if(lmb_source_close(fd))bad=1;
    return bad?-1:0;
}
static inline int lmb_gguf_import_cached(const char *root,const char *ref,char *directory,size_t capacity) {
    LmbGguf g={0};LmbGgufMetadata metadata={0};LmbGgufJson sidecar={0};
    LmbSourceStat *sources=NULL;char output[1536]="",temporary[1600]="",path[2048],source[2048];
    const char *work=output;
    int created=0,rc=-1;unsigned linked=0;
    if(!ref || strchr(ref,'/') || strchr(ref,'\\') || lmb_gguf_load(root,ref,&g) ||
       lmb_gguf_metadata(&g,&metadata))goto done;
    sources=calloc(g.nfiles,sizeof *sources);if(!sources)goto done;
    LmbSha digest;lmb_sha_init(&digest);
    const char version[]="lumabri-gguf-import-v2";lmb_sha_update(&digest,version,sizeof version);
    /* File identities below, not the spelling of the selected folder, define
     * this cache key. A relative path must reuse the absolute path's import. */
    LmbGgufJson *docs[]={&metadata.tokenizer,&metadata.tokenizer_config,&metadata.generation_config,&sidecar,&metadata.config};
    const char *names[]={"tokenizer.json","tokenizer_config.json","generation_config.json","gguf.json","config.json"};
    if(g.nfiles==1)lmb_gguf_json_printf(&sidecar,"{\"format\":\"colibri-gguf-v1\",\"gguf\":\"weights.gguf\"}");
    else lmb_gguf_json_printf(&sidecar,"{\"format\":\"colibri-gguf-v1\",\"gguf\":\"weights-00001-of-%05u.gguf\"}",g.nfiles);
    if(sidecar.bad)goto done;
    for(unsigned i=0;i<5;i++){lmb_sha_le64(&digest,docs[i]->length);lmb_sha_update(&digest,docs[i]->data,docs[i]->length);}
    for(uint32_t i=0;i<g.nfiles;i++) {
        int n=snprintf(source,sizeof source,"%s/%s",root,g.files[i]);
        if(n<0 || (size_t)n>=sizeof source || lmb_source_stat(source,&sources[i]) || !sources[i].regular)goto done;
        lmb_sha_update(&digest,g.files[i],strlen(g.files[i])+1);
        lmb_sha_le64(&digest,sources[i].device);lmb_sha_le64(&digest,sources[i].inode);
        lmb_sha_le64(&digest,sources[i].size);lmb_sha_le64(&digest,sources[i].mtime_ns);
    }
    uint8_t hash[32];char hex[65];lmb_sha_final(&digest,hash);
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",hash[i]);
    int n=snprintf(output,sizeof output,"%s/.lumabri-gguf-%s",root,hex);
    if(n<0 || (size_t)n>=sizeof output || (size_t)n>=capacity)goto done;
    LmbSourceStat existing;
    if(!lmb_source_stat(output,&existing)) {
        if(!existing.directory || existing.symlink)goto done;
    } else {
        if(errno!=ENOENT || lmb_gguf_import_temporary(output,temporary,sizeof temporary))goto done;
        created=1;work=temporary;
    }
    for(uint32_t i=0;i<g.nfiles;i++) {
        char file[64];
        if(g.nfiles==1)strcpy(file,"weights.gguf");
        else snprintf(file,sizeof file,"weights-%05u-of-%05u.gguf",i+1,g.nfiles);
        snprintf(source,sizeof source,"%s/%s",root,g.files[i]);snprintf(path,sizeof path,"%s/%s",work,file);
        if(created) {if(lmb_gguf_link(source,path))goto done;linked++;}
        LmbSourceStat actual;
        if(lmb_source_stat(path,&actual) || actual.symlink || !actual.regular || actual.size!=sources[i].size ||
           actual.device!=sources[i].device || actual.inode!=sources[i].inode || actual.mtime_ns!=sources[i].mtime_ns)goto done;
    }
    /* Build privately, then publish the complete directory atomically. A crash
     * leaves only an unused temporary directory, never a poisoned cache key. */
    for(unsigned i=0;i<5;i++) {
        snprintf(path,sizeof path,"%s/%s",work,names[i]);
        if(lmb_gguf_file_text(path,docs[i],created))goto done;
    }
    LmbModelShape shape;
    if(lmb_shape_from_config(work,&shape) || !shape.sizing_verified)goto done;
    if(created && lmb_source_rename(work,output))goto done;
    created=0;
    strcpy(directory,output);rc=0;
done:
    if(rc && created) {
        const char *files[]={"config.json","tokenizer.json","tokenizer_config.json","generation_config.json","gguf.json"};
        for(unsigned i=0;i<5;i++){snprintf(path,sizeof path,"%s/%s",work,files[i]);(void)lmb_source_unlink(path);}
        for(unsigned i=0;i<linked;i++) {
            if(g.nfiles==1)snprintf(path,sizeof path,"%s/weights.gguf",work);
            else snprintf(path,sizeof path,"%s/weights-%05u-of-%05u.gguf",work,i+1,g.nfiles);
            (void)lmb_source_unlink(path);
        }
        lmb_gguf_import_rmdir(work);
    }
    free(sources);free(sidecar.data);lmb_gguf_metadata_free(&metadata);lmb_gguf_free(&g);return rc;
}
#endif
