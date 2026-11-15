/* lumabri-fetch: downloads one model that a person asked for.
 *
 * A separate small program so that the household runtimes keep no TLS or
 * curl dependency. `lumabri fetch` and the local web gateway start it with
 * one catalogue entry; it trusts none of it and checks every value again.
 *
 * A model is a snapshot of a repository at one commit:
 *   1. a branch name is resolved to a commit once and written beside the
 *      model, so that a resumed download continues the same snapshot;
 *   2. the file list of that commit gives every file's size and content hash
 *      (SHA-256 for large files, git's blob SHA-1 for the small ones);
 *   3. each file is fetched into a hidden folder inside the models folder,
 *      resuming with an HTTP Range request, and takes its name only when its
 *      size and hash match;
 *   4. the folder gets the model's name only when every file is there, so an
 *      incomplete or wrong model never reaches the model catalogue.
 * A catalogue entry can instead name its files, with their sizes and SHA-256,
 * for a repository that holds many variants: then only those are fetched, no
 * list is asked for, and the checked GGUF files are placed in the models
 * folder itself.
 *
 * HTTPS with certificate and host verification; there is no switch to turn
 * that off. Plain HTTP is accepted only for this computer's loopback address,
 * which is how the tests serve files. Redirects are followed by hand, each
 * one checked, at most LMB_DOWNLOAD_REDIRECTS times. No credentials are sent.
 * One download per computer, held by a lock in ~/.lumabri/downloads. */
#define _GNU_SOURCE
#include "src/runtime/lumabri_download.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "lumabri_sha.h"
#pragma GCC diagnostic pop
#include <curl/curl.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>

enum { STOP_NONE=0, STOP_CANCEL, STOP_PAUSE };
#define FETCH_DOCUMENT_MAX (8u << 20)
#define FETCH_PAGES 64u

/* ---- git's blob hash for files a repository stores itself: SHA-1, FIPS 180-4.
 * It detects damage and a wrong file. It is not collision resistant; the
 * large files, which are the weights, are checked with SHA-256. */
typedef struct { uint32_t h[5]; uint64_t length; uint8_t block[64]; size_t used; } Sha1;
static void sha1_init(Sha1 *s) {
    s->h[0]=0x67452301; s->h[1]=0xefcdab89; s->h[2]=0x98badcfe; s->h[3]=0x10325476; s->h[4]=0xc3d2e1f0;
    s->length=0; s->used=0;
}
static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80], a=s->h[0], b=s->h[1], c=s->h[2], d=s->h[3], e=s->h[4];
    for (unsigned i=0; i<16; i++)
        w[i]=(uint32_t)p[4*i]<<24 | (uint32_t)p[4*i+1]<<16 | (uint32_t)p[4*i+2]<<8 | p[4*i+3];
    for (unsigned i=16; i<80; i++) { uint32_t x=w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i]=x<<1 | x>>31; }
    for (unsigned i=0; i<80; i++) {
        uint32_t f=i<20 ? ((b&c)|(~b&d)) : i<40 ? (b^c^d) : i<60 ? ((b&c)|(b&d)|(c&d)) : (b^c^d);
        uint32_t k=i<20 ? 0x5a827999 : i<40 ? 0x6ed9eba1 : i<60 ? 0x8f1bbcdc : 0xca62c1d6;
        uint32_t t=(a<<5 | a>>27)+f+e+k+w[i];
        e=d; d=c; c=b<<30 | b>>2; b=a; a=t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void sha1_update(Sha1 *s, const void *data, size_t n) {
    const uint8_t *p=data; s->length+=n;
    while (n) {
        size_t take=64-s->used; if (take>n) take=n;
        memcpy(s->block+s->used,p,take); s->used+=take; p+=take; n-=take;
        if (s->used==64) { sha1_block(s,s->block); s->used=0; }
    }
}
static void sha1_final(Sha1 *s, char hex[41]) {
    uint64_t bits=s->length*8; uint8_t pad=0x80, zero=0, tail[8];
    sha1_update(s,&pad,1);
    while (s->used!=56) sha1_update(s,&zero,1);
    for (unsigned i=0; i<8; i++) tail[i]=(uint8_t)(bits>>(56-8*i));
    sha1_update(s,tail,8);
    for (unsigned i=0; i<5; i++) snprintf(hex+8*i,9,"%08x",s->h[i]);
}

typedef struct { char *path; uint64_t size; char sha256[65], git[41]; } RepoFile;
typedef struct {
    const char *id, *endpoint, *repo, *revision;
    uint64_t estimate;
    int state_dir, models_dir, stage, show;
    char stage_name[96], commit[41];
    RepoFile *files; unsigned count, current;
    int named; /* the catalogue named the files: no listing, placed in the models folder */
    uint64_t total;
    LmbDownloadState record;
    /* progress */
    uint64_t have, sample_bytes;
    double sample_time, report_time, speed, marker_time;
    int stop;
    /* one HTTP exchange */
    CURL *curl;
    int to_file, checked, discard, range_ignored;
    uint64_t range_start;
    const char *abort_reason;
    char link[2048];
    /* a document kept in memory */
    char *memory; size_t memory_size;
    /* a file */
    int file; uint64_t file_size, offset, received;
    LmbSha sha256; Sha1 sha1;
} Fetch;

static volatile sig_atomic_t fetch_signalled;
static void fetch_signal(int sig) { (void)sig; fetch_signalled=1; }

static double fetch_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
static void fetch_record(Fetch *f, const char *state, const char *error) {
    snprintf(f->record.id,sizeof f->record.id,"%s",f->id);
    snprintf(f->record.state,sizeof f->record.state,"%s",state);
    snprintf(f->record.error,sizeof f->record.error,"%s",error ? error : "");
    f->record.bytes=f->have; f->record.total=f->total ? f->total : f->estimate; f->record.speed=(uint64_t)f->speed;
    (void)lmb_download_state_write(f->state_dir,&f->record);
}
/* A person's request to stop arrives as a marker file or a signal. */
static int fetch_stop(Fetch *f) {
    if (f->stop) return f->stop;
    double now=fetch_now();
    if (fetch_signalled) f->stop=STOP_PAUSE;
    else if (now-f->marker_time>=0.2) {
        f->marker_time=now;
        if (lmb_download_marked(f->state_dir,LMB_DOWNLOAD_CANCEL)) f->stop=STOP_CANCEL;
        else if (lmb_download_marked(f->state_dir,LMB_DOWNLOAD_PAUSE)) f->stop=STOP_PAUSE;
    }
    return f->stop;
}
static void fetch_progress(Fetch *f, int force) {
    double now=fetch_now();
    if (now-f->sample_time>=1.0) {
        double rate=f->have>f->sample_bytes ? (double)(f->have-f->sample_bytes)/(now-f->sample_time) : 0;
        f->speed=f->speed>0 ? f->speed*0.6+rate*0.4 : rate;
        f->sample_time=now; f->sample_bytes=f->have;
    }
    if (!force && now-f->report_time<0.4) return;
    f->report_time=now;
    fetch_record(f,"downloading",NULL);
    if (f->show && f->total) {
        fprintf(stderr,"\r%7.2f / %.2f GB  %6.2f MB/s  file %u of %u   ",(double)f->have/1e9,(double)f->total/1e9,
            f->speed/1e6,f->current+1,f->count);
        fflush(stderr);
    }
}

static size_t fetch_header(char *data, size_t unit, size_t count, void *opaque) {
    Fetch *f=opaque; size_t n=unit*count;
    /* Link: <address>; rel="next", <address>; rel="other" */
    char line[2200];
    if (n<6 || n>=sizeof line) return n;
    memcpy(line,data,n); line[n]=0;
    if (!strncasecmp(line,"content-range:",14)) {
        unsigned long long start=0;
        if (sscanf(line+14," bytes %llu-",&start)==1) f->range_start=start;
        return n;
    }
    if (strncasecmp(line,"link:",5)) return n;
    for (char *open=strchr(line,'<'); open; ) {
        char *close=strchr(open,'>'); if (!close) break;
        char *end=strchr(close,','), *next=end ? strchr(end,'<') : NULL;
        if (end) *end=0;
        if (strstr(close,"rel=\"next\"") && (size_t)(close-open-1)<sizeof f->link) {
            memcpy(f->link,open+1,(size_t)(close-open-1)); f->link[close-open-1]=0;
        }
        open=next;
    }
    return n;
}
static size_t fetch_write(char *data, size_t unit, size_t count, void *opaque) {
    Fetch *f=opaque; size_t n=unit*count;
    if (!f->checked) {
        long code=0; curl_off_t length=-1;
        curl_easy_getinfo(f->curl,CURLINFO_RESPONSE_CODE,&code);
        curl_easy_getinfo(f->curl,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&length);
        f->checked=1;
        if (code>=300 && code<400) f->discard=1; /* the body of a redirect is not the file */
        /* Asked to continue at an offset, the server sent something else:
         * never append it. The file starts again. */
        else if (f->to_file && f->offset && (code==200 || (code==206 && f->range_start!=f->offset))) {
            f->range_ignored=1; return 0;
        }
        else if (code!=(f->to_file && f->offset ? 206 : 200)) { f->abort_reason="unexpected_http_status"; return 0; }
        else if (f->to_file && length>=0 && (uint64_t)length+f->offset!=f->file_size) { f->abort_reason="size_mismatch"; return 0; }
    }
    if (f->discard) return n;
    if (!f->to_file) {
        if (n>FETCH_DOCUMENT_MAX-f->memory_size) { f->abort_reason="listing_too_large"; return 0; }
        char *grown=realloc(f->memory,f->memory_size+n+1);
        if (!grown) { f->abort_reason="out_of_memory"; return 0; }
        f->memory=grown; memcpy(grown+f->memory_size,data,n); f->memory_size+=n; grown[f->memory_size]=0;
        return n;
    }
    if (n>f->file_size-f->offset-f->received) { f->abort_reason="size_mismatch"; return 0; }
    size_t at=0;
    while (at<n) {
        ssize_t wrote=write(f->file,data+at,n-at);
        if (wrote<0 && errno==EINTR) continue;
        if (wrote<=0) { f->abort_reason=errno==ENOSPC ? "insufficient_disk_space" : "write_failed"; return 0; }
        at+=(size_t)wrote;
    }
    lmb_sha_update(&f->sha256,data,n); sha1_update(&f->sha1,data,n);
    f->received+=n; f->have+=n;
    return n;
}
static int fetch_transfer_info(void *opaque, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt; (void)dn; (void)ut; (void)un;
    Fetch *f=opaque;
    if (fetch_stop(f)) return 1;
    fetch_progress(f,0);
    return 0;
}

enum { XFER_OK=0, XFER_STOPPED, XFER_RETRY, XFER_RESTART, XFER_FATAL };
/* One attempt from the first address, following checked redirects. A file's
 * repository address is used again for every attempt because the signed
 * addresses it redirects to expire. */
static int fetch_attempt(Fetch *f, const char *first, const char **reason) {
    char url[2048], error[CURL_ERROR_SIZE];
    if (strlen(first)>=sizeof url) { *reason="url_invalid"; return XFER_FATAL; }
    strcpy(url,first);
    for (unsigned hop=0; ; hop++) {
        int loopback=0;
        const char *refused=lmb_download_url_refusal(url,&loopback);
        if (refused) { *reason=refused; return XFER_FATAL; }
        f->checked=0; f->discard=0; f->abort_reason=NULL; f->link[0]=0; error[0]=0;
        f->range_ignored=0; f->range_start=UINT64_MAX;
        if (!f->to_file) { free(f->memory); f->memory=NULL; f->memory_size=0; }
        CURL *c=curl_easy_init();
        if (!c) { *reason="downloader_unavailable"; return XFER_FATAL; }
        f->curl=c;
        curl_easy_setopt(c,CURLOPT_URL,url);
        /* One scheme only. libcurl older than 7.85 has just the bitmask form. */
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,loopback ? "http" : "https");
#else
        curl_easy_setopt(c,CURLOPT_PROTOCOLS,(long)(loopback ? CURLPROTO_HTTP : CURLPROTO_HTTPS));
#endif
        /* Verification is unconditional: no option, variable or catalogue
         * field reaches these two lines. */
        curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);
        curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,2L);
        curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(c,CURLOPT_HTTP_CONTENT_DECODING,0L);
        curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(c,CURLOPT_USERAGENT,"lumabri-fetch/1");
        curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,20L);
        curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,256L);
        curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,60L);
        curl_easy_setopt(c,CURLOPT_ERRORBUFFER,error);
        curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,fetch_write);
        curl_easy_setopt(c,CURLOPT_WRITEDATA,f);
        curl_easy_setopt(c,CURLOPT_HEADERFUNCTION,fetch_header);
        curl_easy_setopt(c,CURLOPT_HEADERDATA,f);
        curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,fetch_transfer_info);
        curl_easy_setopt(c,CURLOPT_XFERINFODATA,f);
        curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
        if (loopback) curl_easy_setopt(c,CURLOPT_PROXY,""); /* never hand a loopback address to a proxy */
        /* The range is a plain request header, so that it also travels through
         * the redirect a repository answers with before its content server. */
        struct curl_slist *headers=NULL; char range[64];
        if (f->to_file && f->offset) {
            snprintf(range,sizeof range,"Range: bytes=%llu-",(unsigned long long)f->offset);
            headers=curl_slist_append(NULL,range);
            if (!headers) { curl_easy_cleanup(c); f->curl=NULL; *reason="out_of_memory"; return XFER_FATAL; }
            curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
        }
        CURLcode rc=curl_easy_perform(c);
        long code=0; char *next=NULL, target[2048]="";
        curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code);
        if (code>=300 && code<400 && !curl_easy_getinfo(c,CURLINFO_REDIRECT_URL,&next) && next &&
            strlen(next)<sizeof target) strcpy(target,next);
        curl_easy_cleanup(c); curl_slist_free_all(headers); f->curl=NULL;
        if (f->stop) return XFER_STOPPED;
        if (f->range_ignored) return XFER_RESTART;
        if (f->abort_reason) { *reason=f->abort_reason; return XFER_FATAL; }
        if (rc==CURLE_OK && code>=300 && code<400) {
            if (!target[0]) { *reason="redirect_invalid"; return XFER_FATAL; }
            if (hop>=LMB_DOWNLOAD_REDIRECTS) { *reason="too_many_redirects"; return XFER_FATAL; }
            refused=lmb_download_redirect_refusal(url,target);
            if (refused) { *reason=refused; return XFER_FATAL; }
            strcpy(url,target); continue;
        }
        if (rc==CURLE_OK) return XFER_OK;
        if (f->show && error[0]) fprintf(stderr,"\n%s\n",error);
        /* The server ignored or refused the range: start the file again. */
        if (f->to_file && f->offset && rc==CURLE_HTTP_RETURNED_ERROR && code==416) return XFER_RESTART;
        if (rc==CURLE_PEER_FAILED_VERIFICATION || rc==CURLE_SSL_CONNECT_ERROR || rc==CURLE_SSL_CACERT_BADFILE ||
            rc==CURLE_SSL_CERTPROBLEM || rc==CURLE_SSL_CIPHER || rc==CURLE_SSL_ISSUER_ERROR ||
            rc==CURLE_SSL_PINNEDPUBKEYNOTMATCH || rc==CURLE_SSL_INVALIDCERTSTATUS) {
            *reason="tls_verification_failed"; return XFER_FATAL;
        }
        if (rc==CURLE_UNSUPPORTED_PROTOCOL) { *reason="url_scheme_refused"; return XFER_FATAL; }
        if (rc==CURLE_HTTP_RETURNED_ERROR && code>=400 && code<500 && code!=408 && code!=429) {
            *reason=code==404 ? "repository_or_file_not_found" : code==401 || code==403 ? "repository_access_denied" :
                "http_client_error";
            return XFER_FATAL;
        }
        *reason=rc==CURLE_COULDNT_RESOLVE_HOST || rc==CURLE_COULDNT_CONNECT ? "network_unreachable" :
            rc==CURLE_OPERATION_TIMEDOUT ? "network_stalled" : rc==CURLE_HTTP_RETURNED_ERROR ? "http_server_error" :
            "transfer_interrupted";
        return XFER_RETRY;
    }
}
/* Sleep in short steps so that a request to stop is still answered. */
static int fetch_wait(Fetch *f, unsigned milliseconds) {
    for (unsigned waited=0; waited<milliseconds; waited+=100) {
        if (fetch_stop(f)) return 1;
        (void)poll(NULL,0,100);
    }
    return fetch_stop(f);
}
/* A small JSON answer into f->memory. 0: read. 1: asked to stop. -1: failed. */
static int fetch_document(Fetch *f, const char *url, const char **reason) {
    f->to_file=0;
    for (unsigned attempt=0; ; attempt++) {
        int rc=fetch_attempt(f,url,reason);
        if (rc==XFER_OK) return f->memory ? 0 : (*reason="listing_invalid", -1);
        if (rc==XFER_STOPPED) return 1;
        if (rc!=XFER_RETRY || attempt>=4) return -1;
        if (fetch_wait(f,250u<<attempt)) return 1;
    }
}
static int fetch_json(Fetch *f, LmbJson *j, LmbJsonToken **tokens) {
    enum { TOKENS=1u<<17 };
    *tokens=calloc(TOKENS,sizeof **tokens);
    return !*tokens || lmb_json_parse(j,f->memory,f->memory_size,*tokens,TOKENS) ? -1 : 0;
}
/* Keep letters, digits, - . _ ~ and the separators of a path. */
static int fetch_quote(char *out, size_t cap, const char *text) {
    size_t at=0;
    for (const unsigned char *p=(const unsigned char *)text; *p; p++) {
        int plain=(*p>='A' && *p<='Z') || (*p>='a' && *p<='z') || (*p>='0' && *p<='9') ||
            *p=='-' || *p=='.' || *p=='_' || *p=='~' || *p=='/';
        if (at+4>=cap) return -1;
        if (plain) out[at++]=(char)*p; else at+=(size_t)snprintf(out+at,4,"%%%02X",*p);
    }
    out[at]=0; return 0;
}

/* The commit this download belongs to: the one already recorded for an
 * unfinished download of the same repository, the pinned one, or the branch
 * resolved now, once. */
static int fetch_commit(Fetch *f, const char **reason) {
    char repo[128], commit[41], url[1024];
    int recorded=lmb_download_revision_read(f->models_dir,f->id,repo,commit);
    int pinned=lmb_download_commit_valid(f->revision);
    struct stat st; int staged=!fstatat(f->models_dir,f->stage_name,&st,AT_SYMLINK_NOFOLLOW);
    if (!recorded && staged && !strcmp(repo,f->repo) && (!pinned || !strcmp(commit,f->revision))) {
        strcpy(f->commit,commit); return 0;
    }
    /* Files of another snapshot are not mixed into this one. */
    lmb_download_discard(f->models_dir,f->id);
    if (pinned) strcpy(f->commit,f->revision);
    else {
        if (snprintf(url,sizeof url,"%s/api/models/%s/revision/%s",f->endpoint,f->repo,f->revision)>=(int)sizeof url) {
            *reason="url_invalid"; return -1;
        }
        int rc=fetch_document(f,url,reason);
        if (rc) return rc;
        LmbJson j; LmbJsonToken *tokens=NULL; int sha=-1;
        if (fetch_json(f,&j,&tokens) || (sha=lmb_download_json_member(&j,0,"sha"))<0 ||
            lmb_api_json_string(&j,(unsigned)sha,f->commit,sizeof f->commit) || !lmb_download_commit_valid(f->commit)) {
            free(tokens); f->commit[0]=0; *reason="revision_answer_invalid"; return -1;
        }
        free(tokens);
    }
    if (lmb_download_revision_write(f->models_dir,f->id,f->repo,f->commit)) { *reason="models_folder_not_writable"; return -1; }
    return 0;
}
static int fetch_listing_page(Fetch *f, const char **reason) {
    LmbJson j; LmbJsonToken *tokens=NULL;
    if (fetch_json(f,&j,&tokens) || tokens[0].kind!=LMB_JSON_ARRAY) { free(tokens); *reason="listing_invalid"; return -1; }
    int bad=0;
    for (unsigned p=1; !bad && p<tokens[0].next; p=tokens[p].next) {
        char type[16]="", path[LMB_DOWNLOAD_PATH];
        int kind=lmb_download_json_member(&j,p,"type");
        if (kind<0 || lmb_api_json_string(&j,(unsigned)kind,type,sizeof type)) { bad=1; break; }
        if (strcmp(type,"file")) continue;
        int name=lmb_download_json_member(&j,p,"path"), size=lmb_download_json_member(&j,p,"size");
        int lfs=lmb_download_json_member(&j,p,"lfs"), oid=lmb_download_json_member(&j,p,"oid");
        RepoFile file; memset(&file,0,sizeof file);
        if (name<0 || lmb_api_json_string(&j,(unsigned)name,path,sizeof path) || lmb_download_json_u64(&j,size,&file.size)) { bad=1; break; }
        if (!lmb_download_path_valid(path)) { *reason="listed_path_refused"; free(tokens); return -1; }
        if (lfs>=0 && tokens[lfs].kind==LMB_JSON_OBJECT) {
            int digest=lmb_download_json_member(&j,(unsigned)lfs,"oid");
            if (digest<0 || lmb_api_json_string(&j,(unsigned)digest,file.sha256,sizeof file.sha256) ||
                !lmb_download_hex(file.sha256,64)) { bad=1; break; }
        } else if (oid>=0 && (lmb_api_json_string(&j,(unsigned)oid,file.git,sizeof file.git) || !lmb_download_hex(file.git,40))) {
            bad=1; break;
        }
        for (unsigned i=0; i<f->count; i++) if (!strcmp(f->files[i].path,path)) bad=1;
        if (bad || f->count==LMB_DOWNLOAD_FILES || file.size>UINT64_MAX/2-f->total) { bad=1; break; }
        RepoFile *grown=realloc(f->files,(f->count+1)*sizeof *grown);
        if (!grown) { bad=1; break; }
        f->files=grown;
        if (!(file.path=strdup(path))) { bad=1; break; }
        f->files[f->count++]=file; f->total+=file.size;
    }
    free(tokens);
    if (bad) { *reason="listing_invalid"; return -1; }
    return 0;
}
/* Every file of the commit, over as many pages as the server sends. A next
 * page is accepted only from the same server as the first. */
static int fetch_listing(Fetch *f, const char **reason) {
    char url[2048];
    if (snprintf(url,sizeof url,"%s/api/models/%s/tree/%s?recursive=true",f->endpoint,f->repo,f->commit)>=(int)sizeof url) {
        *reason="url_invalid"; return -1;
    }
    size_t origin=strlen(f->endpoint);
    for (unsigned page=0; ; page++) {
        if (page==FETCH_PAGES) { *reason="listing_too_large"; return -1; }
        int rc=fetch_document(f,url,reason);
        if (rc) return rc;
        char next[2048]; snprintf(next,sizeof next,"%s",f->link);
        if (fetch_listing_page(f,reason)) return -1;
        if (!next[0]) break;
        int n=next[0]=='/' && next[1]!='/' ? snprintf(url,sizeof url,"%s%s",f->endpoint,next) :
            !strncmp(next,f->endpoint,origin) && next[origin]=='/' ? snprintf(url,sizeof url,"%s",next) : -1;
        if (n<0 || (size_t)n>=sizeof url) { *reason="listing_pagination_refused"; return -1; }
    }
    if (!f->count) { *reason="repository_empty"; return -1; }
    return 0;
}

/* The folder inside the hidden download folder that holds `path`, creating
 * what is missing. Each step is opened without following links. */
static int fetch_parent(Fetch *f, const char *path, const char **leaf) {
    int dir=dup(f->stage); char part[LMB_DOWNLOAD_PATH];
    const char *at=path;
    for (const char *slash; dir>=0 && (slash=strchr(at,'/')); at=slash+1) {
        memcpy(part,at,(size_t)(slash-at)); part[slash-at]=0;
        if (mkdirat(dir,part,0755) && errno!=EEXIST) { close(dir); return -1; }
        int next=openat(dir,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(dir); dir=next;
    }
    *leaf=at; return dir;
}
static void fetch_hash_start(Fetch *f, uint64_t size) {
    char header[32]; int n=snprintf(header,sizeof header,"blob %llu",(unsigned long long)size);
    lmb_sha_init(&f->sha256); sha1_init(&f->sha1); sha1_update(&f->sha1,header,(size_t)n+1);
}
/* Bytes of one file already here, complete or not. */
static uint64_t fetch_file_have(Fetch *f, const RepoFile *file) {
    const char *leaf; char part[LMB_DOWNLOAD_PATH+16]; struct stat st; uint64_t have=0;
    int dir=fetch_parent(f,file->path,&leaf);
    if (dir<0) return 0;
    snprintf(part,sizeof part,"%s" LMB_DOWNLOAD_PART,leaf);
    if (!fstatat(dir,leaf,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && (uint64_t)st.st_size==file->size) have=file->size;
    else if (!fstatat(dir,part,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && (uint64_t)st.st_size<=file->size)
        have=(uint64_t)st.st_size;
    close(dir); return have;
}
/* 0: the file is in place and checked. 1: asked to stop. -1: failed. */
static int fetch_file(Fetch *f, const RepoFile *file, const char **reason) {
    const char *leaf; char part[LMB_DOWNLOAD_PATH+16], quoted[3*LMB_DOWNLOAD_PATH], url[2048]; struct stat st;
    int dir=fetch_parent(f,file->path,&leaf), rc=-1;
    *reason="models_folder_not_writable";
    if (dir<0) return -1;
    snprintf(part,sizeof part,"%s" LMB_DOWNLOAD_PART,leaf);
    if (!fstatat(dir,leaf,&st,AT_SYMLINK_NOFOLLOW)) {
        /* Renamed only after its check in an earlier run. */
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size==file->size) { close(dir); return 0; }
        if (lmb_download_remove_tree(dir,leaf,0)) { close(dir); return -1; }
    }
    f->file=openat(dir,part,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0644);
    if (f->file<0 || fstat(f->file,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1) goto done;
    uint64_t local=(uint64_t)st.st_size;
    if (local>file->size) { if (ftruncate(f->file,0)) goto done; local=0; }
    fetch_hash_start(f,file->size);
    if (local) {
        /* Resuming: the bytes already here are read once into the hashes. */
        unsigned char *block=malloc(1u<<20); uint64_t fed=0;
        while (block && fed<local) {
            ssize_t got=pread(f->file,block,1u<<20,(off_t)fed);
            if (got<0 && errno==EINTR) continue;
            if (got<=0) break;
            lmb_sha_update(&f->sha256,block,(size_t)got); sha1_update(&f->sha1,block,(size_t)got); fed+=(uint64_t)got;
            if (fetch_stop(f)) { free(block); rc=1; goto done; }
            fetch_progress(f,0);
        }
        free(block);
        if (fed!=local) { *reason="write_failed"; goto done; }
    }
    if (fetch_quote(quoted,sizeof quoted,file->path) ||
        snprintf(url,sizeof url,"%s/%s/resolve/%s/%s",f->endpoint,f->repo,f->commit,quoted)>=(int)sizeof url) {
        *reason="url_invalid"; goto done;
    }
    unsigned idle=0, restarts=0;
    while (local<file->size) {
        if (lseek(f->file,(off_t)local,SEEK_SET)<0) { *reason="write_failed"; goto done; }
        f->to_file=1; f->file_size=file->size; f->offset=local; f->received=0;
        int outcome=fetch_attempt(f,url,reason);
        uint64_t before=local; local+=f->received;
        if (outcome==XFER_STOPPED) { rc=1; goto done; }
        if (outcome==XFER_FATAL) {
            if (!strcmp(*reason,"size_mismatch") || !strcmp(*reason,"unexpected_http_status")) {
                (void)unlinkat(dir,part,0); f->have-=local;
            }
            goto done;
        }
        if (outcome==XFER_RESTART) {
            if (restarts++ || ftruncate(f->file,0)) { *reason="range_not_supported"; goto done; }
            f->have-=local; local=0; fetch_hash_start(f,file->size); continue;
        }
        if (outcome==XFER_OK && local<file->size) {
            (void)unlinkat(dir,part,0); f->have-=local; *reason="size_mismatch"; goto done;
        }
        if (outcome==XFER_RETRY) {
            idle=local>before ? 0 : idle+1;
            if (idle>4) goto done; /* the reason of the last attempt */
            fetch_record(f,"downloading",NULL);
            if (fetch_wait(f,250u<<idle)) { rc=1; goto done; }
        }
    }
    uint8_t digest[32]; char sha256[65], git[41];
    lmb_sha_final(&f->sha256,digest); sha1_final(&f->sha1,git);
    for (unsigned i=0; i<32; i++) snprintf(sha256+2*i,3,"%02x",digest[i]);
    if ((file->sha256[0] && strcmp(file->sha256,sha256)) || (file->git[0] && strcmp(file->git,git))) {
        /* A wrong file is removed, never kept for a later resume. */
        (void)unlinkat(dir,part,0); f->have-=local; *reason="checksum_mismatch"; goto done;
    }
    *reason="write_failed";
    if (fsync(f->file) || renameat(dir,part,dir,leaf)) goto done;
    rc=0;
done:
    if (f->file>=0) { close(f->file); f->file=-1; }
    close(dir); return rc;
}

static int fetch_finish(Fetch *f, int code, const char *state, const char *error) {
    if (f->stage>=0) { close(f->stage); f->stage=-1; }
    if (code==LMB_FETCH_CANCELLED) { lmb_download_discard(f->models_dir,f->id); f->have=0; }
    f->speed=0;
    fetch_record(f,state,error);
    (void)unlinkat(f->state_dir,LMB_DOWNLOAD_CANCEL,0); (void)unlinkat(f->state_dir,LMB_DOWNLOAD_PAUSE,0);
    if (f->show) fputc('\n',stderr);
    if (code==LMB_FETCH_DONE) printf("Downloaded and checked: %s (%u files, revision %s)\n",f->id,f->count,f->commit);
    else if (code==LMB_FETCH_CANCELLED) fprintf(stderr,"Download cancelled; what was fetched has been removed.\n");
    else if (code==LMB_FETCH_PAUSED) fprintf(stderr,"Download stopped; the files already fetched are kept and the next start resumes.\n");
    else fprintf(stderr,"Download failed: %s\n",error ? error : "unknown");
    return code;
}
static int fetch_stopped(Fetch *f) {
    return f->stop==STOP_CANCEL ? fetch_finish(f,LMB_FETCH_CANCELLED,"cancelled",NULL) :
        fetch_finish(f,LMB_FETCH_PAUSED,"interrupted","stopped_before_completion");
}
static int fetch_run(Fetch *f) {
    const char *reason=NULL;
    if (!lmb_download_endpoint_valid(f->endpoint)) {
        reason=lmb_download_url_refusal(f->endpoint,NULL);
        if (!reason) reason="url_invalid";
    }
    if (!reason && !lmb_download_repo_valid(f->repo)) reason="repository_name_refused";
    if (!reason && !lmb_download_revision_valid(f->revision)) reason="revision_name_refused";
    if (reason) return fetch_finish(f,LMB_FETCH_FAILED,"failed",reason);
    struct stat st;
    if (!f->named && !fstatat(f->models_dir,f->id,&st,AT_SYMLINK_NOFOLLOW))
        return fetch_finish(f,LMB_FETCH_FAILED,"failed","model_already_present");
    for (unsigned i=0; f->named && i<f->count; i++)
        if (!fstatat(f->models_dir,f->files[i].path,&st,AT_SYMLINK_NOFOLLOW))
            return fetch_finish(f,LMB_FETCH_FAILED,"failed","model_already_present");
    lmb_download_stage_name(f->stage_name,f->id);
    fetch_record(f,"downloading",NULL);
    int rc=fetch_commit(f,&reason);
    if (rc>0) return fetch_stopped(f);
    if (rc) return fetch_finish(f,LMB_FETCH_FAILED,"failed",reason);
    if (f->show) fprintf(stderr,"Revision %s\n",f->commit);
    if (mkdirat(f->models_dir,f->stage_name,0700) && errno!=EEXIST)
        return fetch_finish(f,LMB_FETCH_FAILED,"failed","models_folder_not_writable");
    f->stage=openat(f->models_dir,f->stage_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (f->stage<0 || fstat(f->stage,&st) || st.st_uid!=geteuid())
        return fetch_finish(f,LMB_FETCH_FAILED,"failed","models_folder_not_writable");
    rc=f->named ? 0 : fetch_listing(f,&reason);
    if (rc>0) return fetch_stopped(f);
    if (rc) return fetch_finish(f,LMB_FETCH_FAILED,"failed",reason);
    for (unsigned i=0; i<f->count; i++) f->have+=fetch_file_have(f,&f->files[i]);
    uint64_t free_bytes=0;
    if (lmb_download_free_bytes(f->models_dir,&free_bytes) || !lmb_download_space_ok(free_bytes,f->total,f->have))
        return fetch_finish(f,LMB_FETCH_FAILED,"failed","insufficient_disk_space");
    f->sample_bytes=f->have; f->sample_time=fetch_now();
    fetch_progress(f,1);
    for (f->current=0; f->current<f->count; f->current++) {
        rc=fetch_file(f,&f->files[f->current],&reason);
        if (rc>0) return fetch_stopped(f);
        if (rc) return fetch_finish(f,LMB_FETCH_FAILED,"failed",reason);
    }
    /* Every file is checked: only now does the model get its name. */
    f->have=f->total;
    if (f->named) {
        /* linkat never replaces a file that appeared meanwhile. The first
         * part of a split GGUF, which the catalogue reads, goes last. */
        for (unsigned i=f->count; i-->0; )
            if (linkat(f->stage,f->files[i].path,f->models_dir,f->files[i].path,0))
                return fetch_finish(f,LMB_FETCH_FAILED,"failed",errno==EEXIST ? "model_already_present" :
                    "models_folder_not_writable");
        close(f->stage); f->stage=-1;
        (void)lmb_download_remove_tree(f->models_dir,f->stage_name,0); (void)fsync(f->models_dir);
        return fetch_finish(f,LMB_FETCH_DONE,"done",NULL);
    }
    if (fchmod(f->stage,0755) || fsync(f->stage) || renameat(f->models_dir,f->stage_name,f->models_dir,f->id))
        return fetch_finish(f,LMB_FETCH_FAILED,"failed",errno==EEXIST || errno==ENOTEMPTY ? "model_already_present" :
            "models_folder_not_writable");
    (void)fsync(f->models_dir);
    return fetch_finish(f,LMB_FETCH_DONE,"done",NULL);
}

static int fetch_usage(void) {
    fputs("lumabri-fetch get --models DIR --state DIR --id ID --endpoint URL --repo OWNER/NAME --revision REV --estimate BYTES\n"
          "                  [--file NAME.gguf:BYTES:SHA256]... [--progress]\n"
          "lumabri-fetch check-url URL [REDIRECT_URL]\n"
          "lumabri-fetch check-path PATH\n"
          "lumabri-fetch version\n"
          "  Started by `lumabri fetch` and by the local web page. HTTPS only, certificates always verified;\n"
          "  plain HTTP is accepted for 127.0.0.1 and [::1] only.\n",stderr);
    return LMB_FETCH_USAGE;
}
int main(int argc, char **argv) {
    if (argc>=3 && argc<=4 && !strcmp(argv[1],"check-url")) {
        const char *refused=argc==4 ? lmb_download_redirect_refusal(argv[2],argv[3]) : lmb_download_url_refusal(argv[2],NULL);
        puts(refused ? refused : "allowed");
        return refused ? LMB_FETCH_FAILED : 0;
    }
    if (argc==3 && !strcmp(argv[1],"check-path")) {
        int valid=lmb_download_path_valid(argv[2]);
        puts(valid ? "allowed" : "listed_path_refused");
        return valid ? 0 : LMB_FETCH_FAILED;
    }
    if (argc==2 && !strcmp(argv[1],"version")) {
        const curl_version_info_data *info=curl_version_info(CURLVERSION_NOW);
        printf("lumabri-fetch 1 · libcurl %s · TLS %s\n",info->version,info->ssl_version ? info->ssl_version : "none");
        return info->ssl_version ? 0 : LMB_FETCH_FAILED;
    }
    if (argc<2 || strcmp(argv[1],"get")) return fetch_usage();
    Fetch f; memset(&f,0,sizeof f); f.state_dir=f.models_dir=f.stage=f.file=-1;
    const char *models=NULL, *state=NULL, *estimate="0";
    for (int i=2; i<argc; i++) {
        if (!strcmp(argv[i],"--progress")) f.show=1;
        else if (i+1>=argc) return fetch_usage();
        else if (!strcmp(argv[i],"--models")) models=argv[++i];
        else if (!strcmp(argv[i],"--state")) state=argv[++i];
        else if (!strcmp(argv[i],"--id")) f.id=argv[++i];
        else if (!strcmp(argv[i],"--endpoint")) f.endpoint=argv[++i];
        else if (!strcmp(argv[i],"--repo")) f.repo=argv[++i];
        else if (!strcmp(argv[i],"--revision")) f.revision=argv[++i];
        else if (!strcmp(argv[i],"--estimate")) estimate=argv[++i];
        else if (!strcmp(argv[i],"--file")) {
            char name[200]; unsigned long long size=0; RepoFile file; memset(&file,0,sizeof file);
            RepoFile *grown=realloc(f.files,(f.count+1)*sizeof *grown);
            if (!grown) return LMB_FETCH_FAILED;
            f.files=grown;
            if (f.count==LMB_DOWNLOAD_NAMED_FILES || sscanf(argv[++i],"%199[^:]:%llu:%64s",name,&size,file.sha256)!=3 ||
                !lmb_download_flat_name_valid(name) || !size || size>UINT64_MAX/16 || !lmb_download_hex(file.sha256,64) ||
                !(file.path=strdup(name))) return fetch_usage();
            for (unsigned k=0; k<f.count; k++) if (!strcmp(f.files[k].path,name)) return fetch_usage();
            file.size=size; f.files[f.count++]=file; f.total+=size; f.named=1;
        }
        else return fetch_usage();
    }
    if (!models || !state || !f.id || !f.endpoint || !f.repo || !f.revision || !lmb_download_id_valid(f.id)) return fetch_usage();
    for (const char *p=estimate; *p; p++) {
        if (*p<'0' || *p>'9' || f.estimate>(UINT64_MAX-(unsigned)(*p-'0'))/10) return fetch_usage();
        f.estimate=f.estimate*10+(unsigned)(*p-'0');
    }
    f.state_dir=lmb_download_state_open(state,1);
    if (f.state_dir<0) { fprintf(stderr,"Cannot use the private download directory %s\n",state); return LMB_FETCH_FAILED; }
    int lock=openat(f.state_dir,LMB_DOWNLOAD_LOCK,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600), held=0;
    /* A status probe can hold the lock for an instant. */
    for (unsigned attempt=0; lock>=0 && attempt<20 && !held; attempt++) {
        if (!flock(lock,LOCK_EX|LOCK_NB)) held=1;
        else if (errno!=EWOULDBLOCK) break;
        else (void)poll(NULL,0,50);
    }
    if (!held) { fprintf(stderr,"Another download is running on this computer.\n"); return LMB_FETCH_BUSY; }
    (void)unlinkat(f.state_dir,LMB_DOWNLOAD_CANCEL,0); (void)unlinkat(f.state_dir,LMB_DOWNLOAD_PAUSE,0);
    struct sigaction action; memset(&action,0,sizeof action); action.sa_handler=fetch_signal;
    sigaction(SIGINT,&action,NULL); sigaction(SIGTERM,&action,NULL); sigaction(SIGHUP,&action,NULL);
    signal(SIGPIPE,SIG_IGN);
    f.models_dir=open(models,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    int rc;
    if (f.models_dir<0) rc=fetch_finish(&f,LMB_FETCH_FAILED,"failed","models_folder_unavailable");
    else if (curl_global_init(CURL_GLOBAL_DEFAULT)) rc=fetch_finish(&f,LMB_FETCH_FAILED,"failed","downloader_unavailable");
    else {
        rc=fetch_run(&f);
        curl_global_cleanup();
    }
    for (unsigned i=0; i<f.count; i++) free(f.files[i].path);
    free(f.files); free(f.memory);
    if (f.models_dir>=0) close(f.models_dir);
    close(lock); close(f.state_dir);
    return rc;
}
