/* Model download: the rules and records shared by the `lumabri` program and
 * the `lumabri-fetch` helper. This header opens no network connection and
 * needs no TLS library; only the helper links libcurl.
 *
 * A download is always asked for by a person. What may be fetched comes from
 * a catalogue the owner of this computer controls, never from an HTTP caller.
 * A model is a snapshot of one repository at one commit. Its files are
 * collected in a hidden folder inside the models folder, and that folder gets
 * the model's name only after every file has been checked. */
#ifndef LMB_DOWNLOAD_H
#define LMB_DOWNLOAD_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lumabri_api_json.h"

#define LMB_DOWNLOAD_MODELS 16u
#define LMB_DOWNLOAD_REDIRECTS 5u
#define LMB_DOWNLOAD_CATALOGUE_MAX (256u << 10)
#define LMB_DOWNLOAD_ENDPOINT "https://huggingface.co"
/* Kept free on the models volume beyond the bytes still to be fetched. */
#define LMB_DOWNLOAD_RESERVE (2000000000ull)
/* Bounds on one repository listing. */
#define LMB_DOWNLOAD_FILES 4096u
#define LMB_DOWNLOAD_PATH 512u
#define LMB_DOWNLOAD_PART ".lumabri-part"
/* Lumabri keeps every weight in memory. Before a model is on disk its exact
 * plan is unknown, so the offer rests on an estimate: the resident weights
 * plus this share for conversation state and working buffers. */
#define LMB_DOWNLOAD_MEMORY_MARGIN_PERCENT 15u

/* Exit codes of the lumabri-fetch helper. */
enum { LMB_FETCH_DONE=0, LMB_FETCH_FAILED=1, LMB_FETCH_USAGE=2, LMB_FETCH_BUSY=3, LMB_FETCH_CANCELLED=4, LMB_FETCH_PAUSED=5 };

#define LMB_DOWNLOAD_NAMED_FILES 8u
/* One file an entry names itself, for a repository that holds many variants. */
typedef struct { char path[200], sha256[65]; uint64_t size; } LmbDownloadFile;
/* The fields of the engine's setup catalogue that describe a download. */
typedef struct {
    char id[65], family[32], name[96], repo[128], revision[65], summary[160];
    char licence[64], licence_url[256];
    /* Why this entry cannot be downloaded yet. Such an entry is never offered. */
    char unavailable[128];
    uint64_t disk_bytes, ram_min_bytes, ram_good_bytes;
    /* Weights Lumabri keeps in memory; the download size when not stated. */
    uint64_t resident_bytes;
    /* With named files only these are fetched, each with its size and
     * SHA-256 from the catalogue, and they are placed in the models folder
     * itself, where the model catalogue looks for GGUF files. Without them
     * the whole repository becomes the folder <models>/<id>. */
    LmbDownloadFile files[LMB_DOWNLOAD_NAMED_FILES];
    unsigned file_count;
} LmbDownloadModel;
typedef struct {
    char endpoint[256];
    LmbDownloadModel models[LMB_DOWNLOAD_MODELS];
    unsigned count;
} LmbDownloadCatalogue;

/* ---- names ---- */
static inline int lmb_download_word(const char *s, size_t max, int upper) {
    size_t n=s ? strlen(s) : 0;
    if (!n || n>max || s[0]=='.' || s[0]=='-' || strstr(s,"..")) return 0;
    for (size_t i=0; i<n; i++) {
        unsigned c=(unsigned char)s[i];
        if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='.' || c=='_' || c=='-' ||
              (upper && c>='A' && c<='Z'))) return 0;
    }
    return 1;
}
/* The id is also the model's folder name inside the models folder. */
static inline int lmb_download_id_valid(const char *id) { return lmb_download_word(id,64,0); }
static inline int lmb_download_revision_valid(const char *revision) { return lmb_download_word(revision,64,1); }
static inline int lmb_download_hex(const char *s, size_t length) {
    if (!s || strlen(s)!=length) return 0;
    for (size_t i=0; i<length; i++)
        if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return 0;
    return 1;
}
static inline int lmb_download_commit_valid(const char *commit) { return lmb_download_hex(commit,40); }
/* owner/name, each a plain word. */
static inline int lmb_download_repo_valid(const char *repo) {
    char copy[128]; size_t n=repo ? strlen(repo) : 0;
    if (!n || n>=sizeof copy) return 0;
    memcpy(copy,repo,n+1);
    char *slash=strchr(copy,'/');
    if (!slash || strchr(slash+1,'/')) return 0;
    *slash=0;
    return lmb_download_word(copy,96,1) && lmb_download_word(slash+1,96,1);
}
/* A path from a repository listing, relative to the model folder. Every part
 * is an ordinary name: nothing absolute, no "." or "..", no backslash, drive
 * letter or control character, and never the suffix of an unfinished file. */
static inline int lmb_download_path_valid(const char *path) {
    size_t n=path ? strlen(path) : 0, part=0; unsigned depth=1;
    static const char suffix[]=LMB_DOWNLOAD_PART;
    if (!n || n>=LMB_DOWNLOAD_PATH || path[0]=='/' || path[n-1]=='/') return 0;
    if (n>=sizeof suffix-1 && !strcmp(path+n-(sizeof suffix-1),suffix)) return 0;
    for (size_t i=0; i<=n; i++) {
        unsigned c=(unsigned char)path[i];
        if (i==n || c=='/') {
            const char *name=path+i-part;
            if (!part || part>200 || (part==1 && name[0]=='.') || (part==2 && name[0]=='.' && name[1]=='.')) return 0;
            if (i<n && ++depth>12) return 0;
            part=0; continue;
        }
        if (c<32 || c==127 || c=='\\' || c==':') return 0;
        part++;
    }
    return 1;
}
/* A named file: one plain GGUF file name, no folder part and not hidden. */
static inline int lmb_download_flat_name_valid(const char *name) {
    size_t n=name ? strlen(name) : 0;
    return n>5 && n<200 && !strcmp(name+n-5,".gguf") && lmb_download_word(name,199,1);
}
/* Hidden names beside the models, invisible to the model catalogue. */
static inline void lmb_download_stage_name(char out[96], const char *id) {
    snprintf(out,96,".lumabri-download-%s",id);
}
static inline void lmb_download_revision_name(char out[112], const char *id) {
    snprintf(out,112,".lumabri-download-%s.revision",id);
}

/* ---- where a download may come from ----
 * HTTPS to any host, with certificate verification that cannot be turned off.
 * Plain HTTP only to this computer's own loopback address, which is how the
 * tests serve files. No credentials in the address. Returns NULL when the
 * address is allowed, otherwise a fixed reason code. */
static inline const char *lmb_download_url_refusal(const char *url, int *loopback) {
    size_t n=url ? strlen(url) : 0;
    if (loopback) *loopback=0;
    if (n<9 || n>=2048) return "url_invalid";
    for (size_t i=0; i<n; i++) {
        unsigned c=(unsigned char)url[i];
        if (c<=32 || c>=127 || c=='\\' || c=='"' || c=='<' || c=='>') return "url_invalid";
    }
    int https=!strncmp(url,"https://",8), http=!https && !strncmp(url,"http://",7);
    if (!https && !http) return "url_scheme_refused";
    const char *host=url+(https ? 8 : 7);
    size_t length=strcspn(host,"/?#");
    if (!length || length>255 || memchr(host,'@',length)) return "url_invalid";
    if (https) {
        for (size_t i=0; i<length; i++) {
            unsigned c=(unsigned char)host[i];
            if (!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') ||
                  c=='.' || c=='-' || c==':' || c=='[' || c==']')) return "url_invalid";
        }
        if (host[0]=='.' || host[0]=='-' || host[0]==':') return "url_invalid";
        return NULL;
    }
    const char *literal=!strncmp(host,"127.0.0.1",9) ? host+9 : !strncmp(host,"[::1]",5) ? host+5 : NULL;
    if (!literal) return "plain_http_refused";
    size_t rest=length-(size_t)(literal-host);
    if (rest) {
        if (literal[0]!=':' || rest<2 || rest>6) return "plain_http_refused";
        for (size_t i=1; i<rest; i++) if (literal[i]<'0' || literal[i]>'9') return "plain_http_refused";
    }
    if (loopback) *loopback=1;
    return NULL;
}
/* A redirect is checked like a first address. It may lead to another HTTPS
 * host, as a repository does to its content servers, but an HTTPS download is
 * never sent down to plain HTTP, not even to loopback. */
static inline const char *lmb_download_redirect_refusal(const char *from, const char *to) {
    int from_loopback=0, to_loopback=0;
    const char *reason=lmb_download_url_refusal(to,&to_loopback);
    if (reason) return reason;
    if (lmb_download_url_refusal(from,&from_loopback)) return "url_invalid";
    return to_loopback && !from_loopback ? "redirect_to_plain_http_refused" : NULL;
}
/* scheme://host[:port] and nothing after it. */
static inline int lmb_download_endpoint_valid(const char *endpoint) {
    if (lmb_download_url_refusal(endpoint,NULL) || strlen(endpoint)>=200) return 0;
    const char *host=strstr(endpoint,"://")+3;
    return !host[strcspn(host,"/?#")];
}

/* ---- catalogue ---- */
/* The value of `key` in the object at `object`, or -1. Unknown members are
 * passed over: this reads answers from a server as well as our own files. */
static inline int lmb_download_json_member(const LmbJson *j, unsigned object, const char *key) {
    if (object>=j->count || j->tokens[object].kind!=LMB_JSON_OBJECT) return -1;
    size_t length=strlen(key);
    for (unsigned p=object+1; p<j->tokens[object].next; ) {
        unsigned value=p+1;
        if (value>=j->count) return -1;
        const LmbJsonToken *name=&j->tokens[p];
        if (name->kind==LMB_JSON_STRING && name->end-name->begin==length && !memcmp(j->text+name->begin,key,length))
            return (int)value;
        p=j->tokens[value].next;
    }
    return -1;
}
static inline int lmb_download_json_u64(const LmbJson *j, int index, uint64_t *value) {
    if (index<0 || (unsigned)index>=j->count || j->tokens[index].kind!=LMB_JSON_NUMBER) return -1;
    const LmbJsonToken *t=&j->tokens[index]; uint64_t n=0;
    if (t->end==t->begin) return -1;
    for (size_t p=t->begin; p<t->end; p++) {
        unsigned c=j->text[p];
        if (c<'0' || c>'9' || n>(UINT64_MAX-(c-'0'))/10) return -1;
        n=n*10+(c-'0');
    }
    *value=n; return 0;
}
/* Sizes are written as in the engine's catalogue: GB of 10^9 bytes, with at
 * most three decimals. Parsed without floating point or the locale. */
static inline int lmb_download_json_gb(const LmbJson *j, int index, uint64_t *bytes) {
    if (index<0 || (unsigned)index>=j->count || j->tokens[index].kind!=LMB_JSON_NUMBER) return -1;
    const LmbJsonToken *t=&j->tokens[index]; uint64_t whole=0, fraction=0; unsigned digits=0; int dot=0;
    for (size_t p=t->begin; p<t->end; p++) {
        unsigned c=j->text[p];
        if (c=='.' && !dot) { dot=1; continue; }
        if (c<'0' || c>'9') return -1;
        if (!dot) { if (whole>100000000) return -1; whole=whole*10+(c-'0'); }
        else if (++digits>3) return -1;
        else fraction=fraction*10+(c-'0');
    }
    while (digits++<3) fraction*=10;
    *bytes=whole*1000000000ull+fraction*1000000ull; return 0;
}
static inline int lmb_download_text_plain(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if ((unsigned char)*s<32 || *s==127) return 0;
    return 1;
}
/* Strict: an unknown field, a duplicate id or an unusable value rejects the
 * whole file. An entry with "unavailable_reason" stays in the list but is
 * never offered or downloaded. */
static inline int lmb_download_catalogue_parse(const char *text, size_t size, LmbDownloadCatalogue *out) {
    memset(out,0,sizeof *out);
    if (!text || !size || size>LMB_DOWNLOAD_CATALOGUE_MAX) return -1;
    enum { TOKENS=1024 };
    LmbJsonToken *tokens=calloc(TOKENS,sizeof *tokens); LmbJson j; int root[3], rc=-1;
    const char *const root_keys[]={"schema","models","endpoint"};
    const char *const keys[]={"id","family","name","repo","disk_gb","ram_min_gb","ram_good_gb",
                              "revision","summary","licence","licence_url","resident_gb","unavailable_reason","files"};
    const char *const file_keys[]={"path","size","sha256"};
    uint32_t schema=0;
    if (!tokens || lmb_json_parse(&j,text,size,tokens,TOKENS) || lmb_json_fields(&j,0,root_keys,3,root) ||
        root[0]<0 || root[1]<0 || lmb_json_uint(&j,(unsigned)root[0],&schema) || schema!=1 ||
        tokens[root[1]].kind!=LMB_JSON_ARRAY) goto done;
    strcpy(out->endpoint,LMB_DOWNLOAD_ENDPOINT);
    if (root[2]>=0 && (lmb_api_json_string(&j,(unsigned)root[2],out->endpoint,sizeof out->endpoint) ||
                       !lmb_download_endpoint_valid(out->endpoint))) goto done;
    for (unsigned p=(unsigned)root[1]+1; p<tokens[root[1]].next; p=tokens[p].next) {
        int f[14];
        if (out->count==LMB_DOWNLOAD_MODELS || lmb_json_fields(&j,p,keys,14,f)) goto done;
        LmbDownloadModel *m=&out->models[out->count];
        /* An entry that cannot be downloaded yet needs no repository: none is invented for it. */
        int available=f[12]<0;
        for (unsigned k=0; k<(available ? 4u : 3u); k++) if (f[k]<0) goto done;
        if (f[13]>=0) {
            uint64_t sum=0;
            if (tokens[f[13]].kind!=LMB_JSON_ARRAY) goto done;
            for (unsigned q=(unsigned)f[13]+1; q<tokens[f[13]].next; q=tokens[q].next) {
                int g[3];
                if (m->file_count==LMB_DOWNLOAD_NAMED_FILES || lmb_json_fields(&j,q,file_keys,3,g) ||
                    g[0]<0 || g[1]<0 || g[2]<0) goto done;
                LmbDownloadFile *file=&m->files[m->file_count];
                if (lmb_api_json_string(&j,(unsigned)g[0],file->path,sizeof file->path) ||
                    !lmb_download_flat_name_valid(file->path) || lmb_download_json_u64(&j,g[1],&file->size) ||
                    !file->size || file->size>UINT64_MAX/16 ||
                    lmb_api_json_string(&j,(unsigned)g[2],file->sha256,sizeof file->sha256) ||
                    !lmb_download_hex(file->sha256,64)) goto done;
                for (unsigned i=0; i<m->file_count; i++) if (!strcmp(m->files[i].path,file->path)) goto done;
                sum+=file->size; m->file_count++;
            }
            if (!m->file_count) goto done;
            m->disk_bytes=sum; /* exact; a stated disk_gb must not contradict it */
        }
        if (available && !m->file_count && (f[4]<0 || f[5]<0 || f[6]<0)) goto done;
        strcpy(m->revision,"main");
        if (!available && (lmb_api_json_string(&j,(unsigned)f[12],m->unavailable,sizeof m->unavailable) ||
                           !lmb_download_text_plain(m->unavailable))) goto done;
        if (lmb_api_json_string(&j,(unsigned)f[0],m->id,sizeof m->id) || !lmb_download_id_valid(m->id) ||
            lmb_api_json_string(&j,(unsigned)f[1],m->family,sizeof m->family) || !lmb_download_text_plain(m->family) ||
            lmb_api_json_string(&j,(unsigned)f[2],m->name,sizeof m->name) || !lmb_download_text_plain(m->name) ||
            (f[3]>=0 && (lmb_api_json_string(&j,(unsigned)f[3],m->repo,sizeof m->repo) || !lmb_download_repo_valid(m->repo))) ||
            (f[4]>=0 && !m->file_count && (lmb_download_json_gb(&j,f[4],&m->disk_bytes) || !m->disk_bytes)) ||
            (f[5]>=0 && lmb_download_json_gb(&j,f[5],&m->ram_min_bytes)) ||
            (f[6]>=0 && lmb_download_json_gb(&j,f[6],&m->ram_good_bytes)) ||
            (f[11]>=0 && (lmb_download_json_gb(&j,f[11],&m->resident_bytes) || !m->resident_bytes)) ||
            (f[7]>=0 && (lmb_api_json_string(&j,(unsigned)f[7],m->revision,sizeof m->revision) ||
                         !lmb_download_revision_valid(m->revision))) ||
            (f[8]>=0 && (lmb_api_json_string(&j,(unsigned)f[8],m->summary,sizeof m->summary) ||
                         !lmb_download_text_plain(m->summary))) ||
            (f[9]>=0 && (lmb_api_json_string(&j,(unsigned)f[9],m->licence,sizeof m->licence) ||
                         !lmb_download_text_plain(m->licence))) ||
            (f[10]>=0 && (lmb_api_json_string(&j,(unsigned)f[10],m->licence_url,sizeof m->licence_url) ||
                          strncmp(m->licence_url,"https://",8) || lmb_download_url_refusal(m->licence_url,NULL)))) goto done;
        if (!m->resident_bytes) m->resident_bytes=m->disk_bytes;
        for (unsigned i=0; i<out->count; i++) {
            if (!strcmp(out->models[i].id,m->id)) goto done;
            for (unsigned a=0; a<m->file_count; a++) for (unsigned b=0; b<out->models[i].file_count; b++)
                if (!strcmp(m->files[a].path,out->models[i].files[b].path)) goto done;
        }
        out->count++;
    }
    rc=0;
done:
    free(tokens);
    if (rc) memset(out,0,sizeof *out);
    return rc;
}
/* Only an entry that can be downloaded is ever found by its id. */
static inline const LmbDownloadModel *lmb_download_find(const LmbDownloadCatalogue *c, const char *id) {
    for (unsigned i=0; i<c->count; i++)
        if (!c->models[i].unavailable[0] && !strcmp(c->models[i].id,id)) return &c->models[i];
    return NULL;
}
static inline uint64_t lmb_download_memory_needed(const LmbDownloadModel *m) {
    return m->resident_bytes+m->resident_bytes/100*LMB_DOWNLOAD_MEMORY_MARGIN_PERCENT;
}

#ifndef _WIN32
/* ---- one download per computer: private directory, lock and progress ----
 * ~/.lumabri/downloads holds a lock the running helper keeps, one progress
 * record replaced atomically, and two request markers. Nothing here is a
 * process id: asking a download to stop never signals another process. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define LMB_DOWNLOAD_LOCK "lock"
#define LMB_DOWNLOAD_RECORD "state"
#define LMB_DOWNLOAD_CANCEL "cancel" /* stop and discard what was fetched */
#define LMB_DOWNLOAD_PAUSE "pause"   /* stop and keep it for a later resume */

typedef struct {
    char id[65], state[16], error[48];
    uint64_t bytes, total, speed, updated;
} LmbDownloadState;

static inline int lmb_download_state_path(char *out, size_t cap) {
    const char *home=getenv("HOME");
    int n=snprintf(out,cap,"%s/.lumabri/downloads",home && *home ? home : ".");
    return n<0 || (size_t)n>=cap ? -1 : 0;
}
/* Owner-only directory that is really a directory and really ours. */
static inline int lmb_download_state_open(const char *path, int create) {
    if (create) {
        char parent[1024]; size_t n=strlen(path);
        if (n>=sizeof parent) return -1;
        memcpy(parent,path,n+1);
        char *slash=strrchr(parent,'/');
        if (slash && slash!=parent) { *slash=0; (void)mkdir(parent,0700); }
        (void)mkdir(path,0700);
    }
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    struct stat st;
    if (fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&077)) { close(fd); return -1; }
    return fd;
}
/* 1: a helper holds the lock. 0: none. -1: cannot tell. */
static inline int lmb_download_running(int dir) {
    int fd=openat(dir,LMB_DOWNLOAD_LOCK,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    int rc=flock(fd,LOCK_SH|LOCK_NB) ? (errno==EWOULDBLOCK ? 1 : -1) : 0;
    close(fd); /* closing releases a lock taken by the probe */
    return rc;
}
static inline int lmb_download_token(const char *s, size_t max) {
    size_t n=strlen(s);
    if (!n || n>max) return 0;
    for (size_t i=0; i<n; i++) {
        unsigned c=(unsigned char)s[i];
        if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='.' || c=='_' || c=='-')) return 0;
    }
    return 1;
}
static inline int lmb_download_write_small(int dir, const char *name, const char *text) {
    char temporary[160]; size_t n=strlen(text), at=0; int bad=0;
    if (snprintf(temporary,sizeof temporary,"%s.new",name)>=(int)sizeof temporary) return -1;
    int fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    while (!bad && at<n) {
        ssize_t wrote=write(fd,text+at,n-at);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) bad=1; else at+=(size_t)wrote;
    }
    if (close(fd)) bad=1;
    if (!bad && renameat(dir,temporary,dir,name)) bad=1;
    if (bad) (void)unlinkat(dir,temporary,0);
    return bad ? -1 : 0;
}
/* 0: read into `text`. 1: there is no such file. -1: unreadable. */
static inline int lmb_download_read_small(int dir, const char *name, char *text, size_t cap) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return errno==ENOENT ? 1 : -1;
    size_t at=0; struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    for (;;) {
        ssize_t got=read(fd,text+at,cap-1-at);
        if (got<0 && errno==EINTR) continue;
        if (got<=0 || (at+=(size_t)got)==cap-1) break;
    }
    close(fd); text[at]=0; return 0;
}
static inline int lmb_download_state_write(int dir, const LmbDownloadState *s) {
    char line[320];
    int n=snprintf(line,sizeof line,"LMBDL1 %s %s %llu %llu %llu %llu %s\n",s->id,s->state,
        (unsigned long long)s->bytes,(unsigned long long)s->total,(unsigned long long)s->speed,
        (unsigned long long)time(NULL),s->error[0] ? s->error : "-");
    if (n<0 || (size_t)n>=sizeof line || !lmb_download_token(s->id,64) || !lmb_download_token(s->state,15) ||
        (s->error[0] && !lmb_download_token(s->error,47))) return -1;
    return lmb_download_write_small(dir,LMB_DOWNLOAD_RECORD,line);
}
/* 0: a record was read. 1: there is none. -1: unreadable or malformed. */
static inline int lmb_download_state_read(int dir, LmbDownloadState *s) {
    char line[320]; memset(s,0,sizeof *s);
    int rc=lmb_download_read_small(dir,LMB_DOWNLOAD_RECORD,line,sizeof line);
    if (rc) return rc;
    unsigned long long bytes=0,total=0,speed=0,updated=0; char tail=0;
    if (sscanf(line,"LMBDL1 %64s %15s %llu %llu %llu %llu %47s%c",s->id,s->state,&bytes,&total,&speed,&updated,
               s->error,&tail)!=8 || tail!='\n' || !lmb_download_token(s->id,64) ||
        !lmb_download_token(s->state,15) || !lmb_download_token(s->error,47)) {
        memset(s,0,sizeof *s); return -1;
    }
    if (!strcmp(s->error,"-")) s->error[0]=0;
    s->bytes=bytes; s->total=total; s->speed=speed; s->updated=updated;
    return 0;
}
/* Which commit an unfinished or finished download of `id` belongs to. Kept
 * beside the model, so a resumed download continues the same snapshot. */
static inline int lmb_download_revision_write(int models, const char *id, const char *repo, const char *commit) {
    char name[112], line[256]; lmb_download_revision_name(name,id);
    if (!lmb_download_repo_valid(repo) || !lmb_download_commit_valid(commit)) return -1;
    snprintf(line,sizeof line,"LMBREV1 %s %s\n",repo,commit);
    return lmb_download_write_small(models,name,line);
}
static inline int lmb_download_revision_read(int models, const char *id, char repo[128], char commit[41]) {
    char name[112], line[256], tail=0; lmb_download_revision_name(name,id);
    repo[0]=0; commit[0]=0;
    int rc=lmb_download_read_small(models,name,line,sizeof line);
    if (rc) return rc;
    if (sscanf(line,"LMBREV1 %127s %40s%c",repo,commit,&tail)!=3 || tail!='\n' ||
        !lmb_download_repo_valid(repo) || !lmb_download_commit_valid(commit)) { repo[0]=0; commit[0]=0; return -1; }
    return 0;
}
static inline int lmb_download_mark(int dir, const char *marker) {
    int fd=openat(dir,marker,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return -1;
    return close(fd);
}
static inline int lmb_download_marked(int dir, const char *marker) {
    struct stat st;
    return !fstatat(dir,marker,&st,AT_SYMLINK_NOFOLLOW);
}
/* Remove one of our own hidden folders. Never follows a link, never leaves
 * the folder it was given, and stops at a depth no listing can reach. */
static inline int lmb_download_remove_tree(int parent, const char *name, unsigned depth) {
    if (depth>16) return -1;
    if (!unlinkat(parent,name,0) || errno==ENOENT) return 0;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    DIR *d=fdopendir(fd);
    if (!d) { close(fd); return -1; }
    int bad=0;
    for (struct dirent *e; (e=readdir(d)); ) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if (lmb_download_remove_tree(dirfd(d),e->d_name,depth+1)) bad=1;
    }
    closedir(d);
    return bad || unlinkat(parent,name,AT_REMOVEDIR) ? -1 : 0;
}
static inline void lmb_download_discard(int models, const char *id) {
    char stage[96], revision[112];
    lmb_download_stage_name(stage,id); lmb_download_revision_name(revision,id);
    (void)lmb_download_remove_tree(models,stage,0);
    (void)unlinkat(models,revision,0);
}
/* Bytes an unprivileged process may still write on the volume of `fd`. */
static inline int lmb_download_free_bytes(int fd, uint64_t *bytes) {
    struct statvfs v;
    if (fstatvfs(fd,&v)) return -1;
    uint64_t unit=v.f_frsize ? v.f_frsize : v.f_bsize, blocks=v.f_bavail;
    *bytes=unit && blocks>UINT64_MAX/unit ? UINT64_MAX : blocks*unit;
    return 0;
}
/* `have` bytes of the model are already on this volume. */
static inline int lmb_download_space_ok(uint64_t free_bytes, uint64_t size, uint64_t have) {
    uint64_t missing=size>have ? size-have : 0;
    return missing<=UINT64_MAX-LMB_DOWNLOAD_RESERVE && free_bytes>=missing+LMB_DOWNLOAD_RESERVE;
}
#endif
#endif
