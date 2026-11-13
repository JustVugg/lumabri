/* Model download for the `lumabri` program: the `lumabri fetch` command and
 * the /api/v1/downloads endpoint of the loopback gateway.
 *
 * Nothing here opens a network connection. Both entry points read the
 * catalogue of downloadable models and start `lumabri-fetch`, the one helper
 * that links a TLS library, for exactly one entry that a person chose. An
 * HTTP caller can name a catalogue id and nothing else: no address, file
 * name, folder or command.
 *
 * Requires the controller's settings, formatting and API response helpers. */
#ifndef LMB_DOWNLOAD_API_H
#define LMB_DOWNLOAD_API_H
#include "lumabri_download.h"
#include "lumabri_operator_access.h"
#include "build/download_assets.h"

#define LMB_DOWNLOAD_HELPER "lumabri-fetch"
#define LMB_DOWNLOAD_OWNER_CATALOGUE "download-catalogue.json"

/* The built-in catalogue, unless the owner of this computer placed their own
 * at ~/.lumabri/download-catalogue.json or named one on the command line.
 * A catalogue that someone else could have written is refused, not skipped. */
static int download_catalogue_load(const char *file, LmbDownloadCatalogue *out, const char **source) {
    char path[1200];
    *source="built-in";
    if (!file) {
        if (checked_printf(path,sizeof path,"%s/.lumabri/" LMB_DOWNLOAD_OWNER_CATALOGUE,getenv("HOME") ? getenv("HOME") : "."))
            return -1;
        struct stat probe;
        if (lstat(path,&probe)) {
            if (errno!=ENOENT) return -1;
            return lmb_download_catalogue_parse((const char *)lmb_download_catalogue_json,
                sizeof lmb_download_catalogue_json-1,out);
        }
        file=path;
    }
    *source="owner file";
    int fd=open(file,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); struct stat st;
    if (fd<0) return -1;
    char *text=NULL; int rc=-1;
    if (!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&022) &&
        st.st_size>0 && (uint64_t)st.st_size<=LMB_DOWNLOAD_CATALOGUE_MAX && (text=malloc((size_t)st.st_size)) &&
        !lmb_read_full(fd,text,(size_t)st.st_size)) rc=lmb_download_catalogue_parse(text,(size_t)st.st_size,out);
    free(text); close(fd);
    return rc;
}
/* Only the models folder from this computer's settings. Returns its
 * descriptor; a single GGUF file chosen as "folder" is not a place to save. */
static int download_models_open(char *path, size_t cap) {
    HomeSettings settings; home_settings_load(&settings);
    char standard[512];
    if (checked_printf(path,cap,"%s",settings.models)) return -1;
    if (!checked_printf(standard,sizeof standard,"%s/.lumabri/models",getenv("HOME") ? getenv("HOME") : ".") &&
        !strcmp(standard,path)) mkdir_p(path);
    return open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
}
/* The helper beside this program, and proof that it runs here: it is the
 * one program that needs the system's libcurl, which may not be installed. */
static int download_helper_path(char *out, size_t cap) {
    char dir[1024]; exe_dir(dir,sizeof dir);
    if (checked_printf(out,cap,"%s/" LMB_DOWNLOAD_HELPER,dir) || access(out,X_OK)) return -1;
    fflush(NULL);
    pid_t child=fork();
    if (child<0) return -1;
    if (!child) {
        int nullfd=open("/dev/null",O_RDWR);
        if (nullfd<0 || dup2(nullfd,0)<0 || dup2(nullfd,1)<0 || dup2(nullfd,2)<0) _exit(126);
        unsetenv("LUMABRI_TOKEN");
        execl(out,out,"version",(char *)NULL); _exit(127);
    }
    int status=0;
    while (waitpid(child,&status,0)<0) if (errno!=EINTR) return -1;
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}
/* Things in the folder that the model catalogue would look at. */
static unsigned download_local_entries(int models) {
    int copy=dup(models); DIR *d=copy<0 ? NULL : fdopendir(copy); unsigned count=0;
    if (!d) { if (copy>=0) close(copy); return 0; }
    rewinddir(d);
    for (struct dirent *e; (e=readdir(d)); ) if (e->d_name[0]!='.' && count<1000) count++;
    closedir(d); return count;
}
static int download_staged(int models, const LmbDownloadModel *m) {
    char stage[96]; struct stat st; lmb_download_stage_name(stage,m->id);
    return !fstatat(models,stage,&st,AT_SYMLINK_NOFOLLOW);
}
/* 1: the model is in the models folder. 0: nothing with its name is there.
 * -1: something else has one of its names. A repository snapshot is the
 * folder <id>; named files are GGUF files in the folder itself. */
static int download_placed(int models, const LmbDownloadModel *m) {
    struct stat st; unsigned found=0, whole=0;
    if (!m->file_count) return fstatat(models,m->id,&st,AT_SYMLINK_NOFOLLOW) ? 0 : S_ISDIR(st.st_mode) ? 1 : -1;
    for (unsigned i=0; i<m->file_count; i++)
        if (!fstatat(models,m->files[i].path,&st,AT_SYMLINK_NOFOLLOW)) {
            found++;
            if (S_ISREG(st.st_mode) && (uint64_t)st.st_size==m->files[i].size) whole++;
        }
    return !found ? 0 : whole==m->file_count ? 1 : -1;
}
typedef struct { const char *state; const char *error; uint64_t bytes, total, speed; } DownloadView;
/* What a person should be told about one catalogue entry right now. The
 * catalogue size is the published one; the exact total replaces it once the
 * helper has read the repository's file list. */
static DownloadView download_view(int models, const LmbDownloadModel *m, const LmbDownloadState *record, int running) {
    DownloadView v={"absent",NULL,0,m->disk_bytes,0};
    int ours=!strcmp(record->id,m->id);
    int active=!strcmp(record->state,"starting") || !strcmp(record->state,"downloading");
    struct stat st;
    if (ours && record->total) v.total=record->total;
    if (ours && running>0 && active) {
        v.state="downloading"; v.bytes=record->bytes; v.speed=record->speed; return v;
    }
    int placed=download_placed(models,m);
    if (placed) { v.state=placed>0 ? "present" : "conflict"; v.bytes=placed>0 ? v.total : 0; return v; }
    (void)st;
    int staged=download_staged(models,m);
    if (ours && staged) v.bytes=record->bytes;
    if (ours && !strcmp(record->state,"failed")) { v.state="failed"; v.error=record->error[0] ? record->error : "unknown"; }
    else if (staged) v.state="interrupted";
    return v;
}
/* Memory the household's computers offer right now, from the same leased
 * machine reports as the workspace. -1 when they cannot be read. */
static int download_offered_memory(const char *tracker, uint64_t *bytes) {
    LmbMachineReport *nodes=calloc(LMB_INVENTORY_MAX,sizeof *nodes); uint32_t count=0;
    *bytes=0;
    int rc=!nodes || !tracker || !*tracker || lmb_inventory_fetch(tracker,nodes,&count) ? -1 : 0;
    for (uint32_t i=0; !rc && i<count; i++) *bytes+=nodes[i].ram_budget_bytes;
    free(nodes); return rc;
}
/* 1: the estimate fits the offered memory. 0: it does not. -1: unknown. */
static int download_fits(const LmbDownloadModel *m, int offered_known, uint64_t offered) {
    return !offered_known ? -1 : lmb_download_memory_needed(m)<=offered;
}
static int download_list_json(Cap *body, const char *tracker) {
    LmbDownloadCatalogue *c=calloc(1,sizeof *c); const char *source="";
    char models_path[512]="", state_path[1200], helper[1200];
    int models=download_models_open(models_path,sizeof models_path), state=-1, running=0, bad=!c;
    LmbDownloadState record; memset(&record,0,sizeof record);
    if (!lmb_download_state_path(state_path,sizeof state_path) && (state=lmb_download_state_open(state_path,1))>=0) {
        running=lmb_download_running(state);
        if (lmb_download_state_read(state,&record)) memset(&record,0,sizeof record);
    }
    int catalogue_ok=c && !download_catalogue_load(NULL,c,&source);
    uint64_t free_bytes=0; int free_known=models>=0 && !lmb_download_free_bytes(models,&free_bytes);
    uint64_t offered=0; int offered_known=!download_offered_memory(tracker,&offered);
    unsigned unavailable=0;
    for (unsigned i=0; catalogue_ok && i<c->count; i++) if (c->models[i].unavailable[0]) unavailable++;
    if (!bad) bad=cap_str(body,"{\"schema\":1,\"folder\":") || api_json_text(body,models_path) ||
        api_addf(body,",\"folder_ok\":%s,\"local_entries\":%u,\"free_bytes\":",models>=0 ? "true" : "false",
            models>=0 ? download_local_entries(models) : 0) ||
        (free_known ? api_addf(body,"%llu",(unsigned long long)free_bytes) : cap_str(body,"null")) ||
        cap_str(body,",\"offered_memory_bytes\":") ||
        (offered_known ? api_addf(body,"%llu",(unsigned long long)offered) : cap_str(body,"null")) ||
        api_addf(body,",\"memory_margin_percent\":%u,\"reserve_bytes\":%llu,\"downloader_available\":%s,"
            "\"catalogue_ok\":%s,\"catalogue\":\"%s\",\"unavailable_hidden\":%u,\"models\":[",
            LMB_DOWNLOAD_MEMORY_MARGIN_PERCENT,(unsigned long long)LMB_DOWNLOAD_RESERVE,
            download_helper_path(helper,sizeof helper) ? "false" : "true",catalogue_ok ? "true" : "false",
            !strcmp(source,"built-in") ? "built_in" : "owner_file",unavailable);
    const char *active=NULL; unsigned listed=0;
    for (unsigned i=0; !bad && catalogue_ok && models>=0 && i<c->count; i++) {
        const LmbDownloadModel *m=&c->models[i]; char repo[128], commit[41], page[512]="";
        if (m->unavailable[0]) continue;
        int fits=download_fits(m,offered_known,offered);
        DownloadView v=download_view(models,m,&record,running);
        if (lmb_download_revision_read(models,m->id,repo,commit) || strcmp(repo,m->repo)) commit[0]=0;
        if (!strncmp(c->endpoint,"https://",8)) snprintf(page,sizeof page,"%s/%s",c->endpoint,m->repo);
        if (!strcmp(v.state,"downloading")) active=m->id;
        bad=api_addf(body,"%s{\"id\":\"%s\",\"name\":",listed++ ? "," : "",m->id) || api_json_text(body,m->name) ||
            cap_str(body,",\"family\":") || api_json_text(body,m->family) ||
            cap_str(body,",\"repo\":") || api_json_text(body,m->repo) ||
            cap_str(body,",\"revision\":") || api_json_text(body,m->revision) ||
            cap_str(body,",\"commit\":") || (commit[0] ? api_json_text(body,commit) : cap_str(body,"null")) ||
            cap_str(body,",\"summary\":") || api_json_text(body,m->summary) ||
            cap_str(body,",\"licence\":") || (m->licence[0] ? api_json_text(body,m->licence) : cap_str(body,"null")) ||
            cap_str(body,",\"licence_url\":") || (m->licence_url[0] ? api_json_text(body,m->licence_url) : cap_str(body,"null")) ||
            cap_str(body,",\"page_url\":") || (page[0] ? api_json_text(body,page) : cap_str(body,"null")) ||
            api_addf(body,",\"size_bytes\":%llu,\"memory_needed_bytes\":%llu,\"memory_estimated\":true,\"fits\":%s,"
                "\"named_files\":%u,\"state\":\"%s\",\"bytes\":%llu,\"speed_bytes_per_second\":%llu,\"error\":",
                (unsigned long long)v.total,(unsigned long long)lmb_download_memory_needed(m),
                fits<0 ? "null" : fits ? "true" : "false",m->file_count,v.state,
                (unsigned long long)v.bytes,(unsigned long long)v.speed) ||
            (v.error ? api_addf(body,"\"%s\"}",v.error) : cap_str(body,"null}"));
    }
    if (!bad) bad=cap_str(body,"],\"active\":") || (active ? api_addf(body,"\"%s\"}\n",active) : cap_str(body,"null}\n"));
    if (models>=0) close(models);
    if (state>=0) close(state);
    free(c); return bad ? -1 : 0;
}
/* Start the helper for one entry. Foreground: wait and return its exit code.
 * Detached: a new session, so that it outlives the request that asked for it;
 * returns 0 once it was handed over. The household key is not passed on. */
static int download_spawn(const LmbDownloadCatalogue *c, const LmbDownloadModel *m, const char *models,
                          const char *state_path, int detached) {
    char helper[1200], size[24], named[LMB_DOWNLOAD_NAMED_FILES][300];
    if (download_helper_path(helper,sizeof helper)) return -1;
    snprintf(size,sizeof size,"%llu",(unsigned long long)m->disk_bytes);
    char *args[20+2*LMB_DOWNLOAD_NAMED_FILES]; unsigned n=0;
    args[n++]=helper; args[n++]="get"; args[n++]="--models"; args[n++]=(char *)models;
    args[n++]="--state"; args[n++]=(char *)state_path; args[n++]="--id"; args[n++]=(char *)m->id;
    args[n++]="--endpoint"; args[n++]=(char *)c->endpoint; args[n++]="--repo"; args[n++]=(char *)m->repo;
    args[n++]="--revision"; args[n++]=(char *)m->revision; args[n++]="--estimate"; args[n++]=size;
    for (unsigned i=0; i<m->file_count; i++) {
        snprintf(named[i],sizeof named[i],"%s:%llu:%s",m->files[i].path,(unsigned long long)m->files[i].size,m->files[i].sha256);
        args[n++]="--file"; args[n++]=named[i];
    }
    if (!detached && isatty(2)) args[n++]="--progress";
    args[n]=NULL;
    fflush(NULL);
    pid_t child=fork();
    if (child<0) return -1;
    if (!child) {
        unsetenv("LUMABRI_TOKEN");
        if (detached) {
            if (setsid()<0) _exit(125);
            pid_t worker=fork();
            if (worker<0) _exit(125);
            if (worker>0) _exit(0);
            signal(SIGHUP,SIG_IGN); signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL); signal(SIGALRM,SIG_DFL);
            char log[1300];
            int limit=getdtablesize();
            for (int fd=3; fd<limit; fd++) close(fd);
            int nullfd=open("/dev/null",O_RDONLY);
            int logfd=checked_printf(log,sizeof log,"%s/fetch.log",state_path) ? -1 :
                open(log,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
            if (nullfd<0 || logfd<0 || dup2(nullfd,0)<0 || dup2(logfd,1)<0 || dup2(logfd,2)<0) _exit(125);
            if (nullfd>2) close(nullfd);
            if (logfd>2) close(logfd);
        }
        execv(helper,args); _exit(127);
    }
    void (*interrupt)(int)=detached ? SIG_DFL : signal(SIGINT,SIG_IGN); /* the helper answers Ctrl-C itself */
    int status=0;
    while (waitpid(child,&status,0)<0) if (errno!=EINTR) { status=-1; break; }
    if (!detached) signal(SIGINT,interrupt);
    if (status<0 || !WIFEXITED(status)) return -1;
    return detached ? (WEXITSTATUS(status) ? -1 : 0) : WEXITSTATUS(status);
}
/* 0 with *code NULL: may start. Otherwise *code says why not. */
static void download_start_check(const LmbDownloadModel *m, int models, int state, const char *tracker,
                                 const char **code, unsigned *status) {
    struct stat st; uint64_t free_bytes=0; char helper[1200]; LmbDownloadState record;
    *code=NULL; *status=409;
    if (lmb_download_state_read(state,&record)) memset(&record,0,sizeof record);
    DownloadView v=download_view(models,m,&record,0);
    (void)st;
    if (download_placed(models,m)) *code="model_already_present";
    else if (lmb_download_running(state)) *code="download_already_running";
    else if (download_helper_path(helper,sizeof helper)) { *code="downloader_unavailable"; *status=503; }
    else {
        /* Never fetch what the household could not then hold in memory. */
        uint64_t offered=0; int known=!download_offered_memory(tracker,&offered);
        int fits=download_fits(m,known,offered);
        if (fits<0) { *code="household_memory_unknown"; *status=503; }
        else if (!fits) *code="model_too_large_for_household";
    }
    if (*code) return;
    /* Against the published size; the helper checks the exact total again. */
    if (lmb_download_free_bytes(models,&free_bytes) ||
             !lmb_download_space_ok(free_bytes,v.total,v.bytes)) *code="insufficient_disk_space";
}
/* Ask the running helper to stop: `cancel` discards the unfinished file,
 * `pause` keeps it. Returns 1 when a helper was running and was asked. */
static int download_request_stop(int state, const char *marker) {
    if (lmb_download_running(state)<=0) return 0;
    return lmb_download_mark(state,marker) ? -1 : 1;
}
/* Called when the household services are stopped: no transfer keeps running
 * behind a stopped Lumabri. The unfinished file stays for a later resume. */
static void download_pause_for_service_stop(void) {
    char path[1200];
    if (lmb_download_state_path(path,sizeof path)) return;
    int state=lmb_download_state_open(path,0);
    if (state<0) return;
    if (download_request_stop(state,LMB_DOWNLOAD_PAUSE)>0)
        for (unsigned i=0; i<50 && lmb_download_running(state)>0; i++) (void)poll(NULL,0,100);
    close(state);
}
/* With no helper running, cancelling means: forget what was fetched so far.
 * A finished model and its revision record are never touched. */
static void download_discard_parts(int models, int state, const LmbDownloadCatalogue *c, const LmbDownloadModel *only) {
    LmbDownloadState record;
    for (unsigned i=0; i<c->count; i++)
        if ((!only || only==&c->models[i]) && download_staged(models,&c->models[i]))
            lmb_download_discard(models,c->models[i].id);
    if (!lmb_download_state_read(state,&record) && strcmp(record.state,"done") &&
        (!only || !strcmp(record.id,only->id))) {
        snprintf(record.state,sizeof record.state,"cancelled"); record.error[0]=0; record.bytes=0; record.speed=0;
        (void)lmb_download_state_write(state,&record);
    }
}

/* ---- /api/v1/downloads ----
 * Returns 1 when the request was for this endpoint and has been answered.
 * Management permission for every method; never served behind the TLS proxy,
 * where the caller may be on another computer. */
static int api_download_route(int fd, int access_dir, const char *tracker, const LmbApiUser *user,
                              const LmbApiHttp *request, int remote) {
    if (strcmp(request->path,"/api/v1/downloads")) return 0;
    int get=!strcmp(request->method,"GET") && !request->length;
    int post=!strcmp(request->method,"POST") && request->has_length && request->length && request->json;
    if (remote) { api_error(fd,403,"downloads_local_gateway_only"); return 1; }
    if (!lmb_management_allows(access_dir,user)) { api_error(fd,403,"management_permission_required"); return 1; }
    if (!get && !post) { api_error(fd,400,"unsupported_endpoint_or_framing"); return 1; }
    char action[16]="", id[65]="";
    if (post) {
        char body[257]; size_t at=0; uint64_t deadline=lmb_io_monotonic_ms()+5000;
        if (request->length>=sizeof body) { api_error(fd,413,"management_request_too_large"); return 1; }
        while (at<request->length && lmb_io_monotonic_ms()<deadline) {
            ssize_t got=recv(fd,body+at,request->length-at,0);
            if (got<0 && errno==EINTR) continue;
            if (got<=0) break;
            at+=(size_t)got;
        }
        if (at!=request->length) { api_error(fd,408,"request_body_timeout"); return 1; }
        LmbJsonToken tokens[8]; LmbJson j; int f[2]; const char *const keys[]={"action","id"};
        if (lmb_json_parse(&j,body,at,tokens,8) || lmb_json_fields(&j,0,keys,2,f) || f[0]<0 || f[1]<0 ||
            lmb_api_json_string(&j,(unsigned)f[0],action,sizeof action) ||
            lmb_api_json_string(&j,(unsigned)f[1],id,sizeof id) || !lmb_download_id_valid(id) ||
            (strcmp(action,"start") && strcmp(action,"cancel"))) { api_error(fd,400,"invalid_download_request"); return 1; }
    }
    unsigned status=200;
    if (post) {
        LmbDownloadCatalogue *c=calloc(1,sizeof *c); const char *source, *code=NULL;
        char models_path[512], state_path[1200];
        int models=download_models_open(models_path,sizeof models_path);
        int state=lmb_download_state_path(state_path,sizeof state_path) ? -1 : lmb_download_state_open(state_path,1);
        const LmbDownloadModel *m=NULL;
        status=409;
        if (!c || state<0) { code="download_state_unavailable"; status=503; }
        else if (download_catalogue_load(NULL,c,&source)) { code="download_catalogue_invalid"; status=503; }
        else if (!(m=lmb_download_find(c,id))) { code="download_not_found"; status=404; }
        else if (models<0) code="models_folder_unavailable";
        else if (!strcmp(action,"start")) {
            download_start_check(m,models,state,tracker,&code,&status);
            if (!code) {
                LmbDownloadState record; memset(&record,0,sizeof record);
                snprintf(record.id,sizeof record.id,"%s",m->id); snprintf(record.state,sizeof record.state,"starting");
                record.total=m->disk_bytes;
                if (lmb_download_state_write(state,&record) || download_spawn(c,m,models_path,state_path,1)) {
                    code="downloader_unavailable"; status=503;
                } else {
                    fprintf(stderr,"[api-download] user=%s start id=%s\n",user->name,m->id);
                    /* Answer with the helper's own first record when it is quick. */
                    for (unsigned i=0; i<20 && lmb_download_running(state)<=0; i++) (void)poll(NULL,0,50);
                    status=202;
                }
            }
        } else {
            LmbDownloadState record;
            int asked=!lmb_download_state_read(state,&record) && !strcmp(record.id,m->id) ?
                download_request_stop(state,LMB_DOWNLOAD_CANCEL) : 0;
            if (asked<0) { code="download_state_unavailable"; status=503; }
            else if (asked) {
                for (unsigned i=0; i<120 && lmb_download_running(state)>0; i++) (void)poll(NULL,0,50);
                status=202;
            } else if (lmb_download_running(state)>0) code="another_download_is_running";
            else { download_discard_parts(models,state,c,m); status=200; }
            if (!code) fprintf(stderr,"[api-download] user=%s cancel id=%s\n",user->name,m->id);
        }
        if (models>=0) close(models);
        if (state>=0) close(state);
        free(c);
        if (code) { api_error(fd,status,code); return 1; }
    }
    Cap body={0};
    if (download_list_json(&body,tracker)) api_error(fd,500,"allocation_failed"); else (void)api_response(fd,status,body.p);
    free(body.p); return 1;
}

/* ---- lumabri fetch ---- */
static const char *download_error_text(const char *code) {
    static const struct { const char *code, *text; } known[]={
        {"checksum_mismatch","a downloaded file did not match its published checksum and was removed"},
        {"size_mismatch","the server offered a file of a different size than its listing; that file was not kept"},
        {"listed_path_refused","the repository lists a file outside the model folder"},
        {"repository_or_file_not_found","the repository, its revision or one of its files was not found"},
        {"repository_access_denied","the repository refused access"},
        {"insufficient_disk_space","there is not enough free space in the models folder"},
        {"plain_http_refused","the address is not HTTPS"},
        {"redirect_to_plain_http_refused","the server redirected an HTTPS download to plain HTTP"},
        {"too_many_redirects","the server redirected too many times"},
        {"tls_verification_failed","the server's certificate could not be verified"},
        {"network_unreachable","the server could not be reached"},
        {"model_too_large_for_household","the computers of this household do not offer enough memory for this model"},
        {"household_memory_unknown","the memory offered by this household could not be read; start Lumabri first (lumabri start)"},
        {"model_already_present","something with this model's name is already in the models folder"}};
    for (unsigned i=0; i<sizeof known/sizeof *known; i++) if (!strcmp(known[i].code,code)) return known[i].text;
    return code;
}
static int cmd_fetch(int argc, char **argv) {
    const char *action=argc ? argv[0] : "", *catalogue_file=NULL, *id=NULL;
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"--catalogue") && i+1<argc && !catalogue_file) catalogue_file=argv[++i];
        else if (!id && argv[i][0]!='-') id=argv[i];
        else goto usage;
    }
    int get=!strcmp(action,"get"), list=!strcmp(action,"list"), status=!strcmp(action,"status"), cancel=!strcmp(action,"cancel");
    if ((!get && !list && !status && !cancel) || (get ? !id : id!=NULL) || ((status || cancel) && catalogue_file)) goto usage;
    LmbDownloadCatalogue *c=calloc(1,sizeof *c); const char *source="";
    char models_path[512], state_path[1200];
    if (!c) return 1;
    if (download_catalogue_load(catalogue_file,c,&source)) {
        free(c);
        return home_fail("The download catalogue is not usable. It must be a regular file that only you can write, "
            "in the format described in docs/MODEL_DOWNLOAD.md.");
    }
    int models=download_models_open(models_path,sizeof models_path);
    int state=lmb_download_state_path(state_path,sizeof state_path) ? -1 : lmb_download_state_open(state_path,1);
    int rc=1;
    /* The household's offered memory decides what may be fetched. */
    HomeSettings settings; home_settings_load(&settings);
    uint64_t offered=0; int offered_known=0;
    if ((get || list) && !home_settings_activate(&settings) && !home_private_network())
        offered_known=!download_offered_memory(settings.tracker,&offered);
    if (models<0) fprintf(stderr,"The models folder %s is not a folder that can be opened. Choose one in Settings.\n",models_path);
    else if (state<0) fprintf(stderr,"Cannot use the private download directory %s.\n",state_path);
    else if (list || status) {
        LmbDownloadState record; int running=lmb_download_running(state);
        if (lmb_download_state_read(state,&record)) memset(&record,0,sizeof record);
        uint64_t free_bytes=0; (void)lmb_download_free_bytes(models,&free_bytes);
        printf("Models folder: %s (%.1f GB free)\n",models_path,(double)free_bytes/1e9);
        if (list && offered_known) printf("Memory offered by this household: %.1f GB\n",(double)offered/1e9);
        else if (list) puts("Memory offered by this household: unknown (start Lumabri first: lumabri start)");
        unsigned shown=0, unavailable=0;
        for (unsigned i=0; i<c->count; i++) {
            const LmbDownloadModel *m=&c->models[i];
            if (m->unavailable[0]) { unavailable++; continue; }
            DownloadView v=download_view(models,m,&record,running);
            int moving=!strcmp(v.state,"downloading"), fits=download_fits(m,offered_known,offered);
            if (status && !moving && strcmp(v.state,"failed") && strcmp(v.state,"interrupted")) continue;
            shown++;
            printf("%-20s %-12s %7.1f GB  %s\n",m->id,v.state,(double)v.total/1e9,m->name);
            printf("%-20s %s%s%s · %s @ %s\n","",m->summary,m->licence[0] ? " · " : "",m->licence,m->repo,m->revision);
            if (list) printf("%-20s needs about %.0f GB of memory across your computers (estimate)%s\n","",
                (double)lmb_download_memory_needed(m)/1e9,fits>0 ? "" : !fits ? ": too large for this household" : "");
            if (moving || v.bytes)
                printf("%-20s %.2f of %.2f GB%s\n","",(double)v.bytes/1e9,(double)v.total/1e9,
                    moving ? "" : " kept; `lumabri fetch get` resumes");
            if (v.error) printf("%-20s %s\n","",download_error_text(v.error));
        }
        if (status && !shown) puts("No download is running or waiting to be resumed.");
        if (list && !shown) puts("No downloadable model is available yet.");
        if (list && unavailable) printf("%u more %s listed but cannot be downloaded yet.\n",unavailable,unavailable==1 ? "model is" : "models are");
        if (list && shown) puts("Download one that fits with: lumabri fetch get ID");
        rc=0;
    } else if (cancel) {
        int asked=download_request_stop(state,LMB_DOWNLOAD_CANCEL);
        if (asked>0) {
            for (unsigned i=0; i<100 && lmb_download_running(state)>0; i++) (void)poll(NULL,0,100);
            rc=lmb_download_running(state)>0;
            puts(rc ? "The download was asked to stop and has not confirmed yet." : "Download cancelled; what was fetched has been removed.");
        } else if (!asked) { download_discard_parts(models,state,c,NULL); puts("No download is running. Unfinished downloads were removed."); rc=0; }
    } else {
        const LmbDownloadModel *m=lmb_download_find(c,id); const char *code=NULL; unsigned unused;
        if (!m) fprintf(stderr,"No model named %s is offered. See: lumabri fetch list\n",id);
        else {
            download_start_check(m,models,state,settings.tracker,&code,&unused);
            if (code && !strcmp(code,"downloader_unavailable"))
                fprintf(stderr,"The " LMB_DOWNLOAD_HELPER " helper is missing beside lumabri, or cannot run on this computer.\n");
            else if (code) fprintf(stderr,"Not started: %s.\n",!strcmp(code,"download_already_running") ?
                "another download is running on this computer" : download_error_text(code));
            else {
                printf("Downloading %s (%.1f GB) from %s/%s into %s/%s\n",m->name,(double)m->disk_bytes/1e9,
                    c->endpoint,m->repo,models_path,m->id);
                fflush(stdout);
                rc=download_spawn(c,m,models_path,state_path,0);
                if (rc<0) { fprintf(stderr,"The " LMB_DOWNLOAD_HELPER " helper could not be run.\n"); rc=1; }
                else if (rc==LMB_FETCH_FAILED) {
                    LmbDownloadState record;
                    if (!lmb_download_state_read(state,&record) && record.error[0])
                        fprintf(stderr,"Reason: %s.\n",download_error_text(record.error));
                } else if (!rc) puts("It will appear in the model catalogue (Prepare models). Nothing was loaded.");
            }
        }
    }
    if (models>=0) close(models);
    if (state>=0) close(state);
    free(c); return rc;
usage:
    fprintf(stderr,"lumabri fetch list [--catalogue FILE]     downloadable models and their state\n"
        "lumabri fetch get ID [--catalogue FILE]   download one model into the models folder; resumes an unfinished download\n"
        "lumabri fetch status                      progress of the running or interrupted download\n"
        "lumabri fetch cancel                      stop the download and remove what was fetched\n"
        "  Nothing is downloaded unless you ask. HTTPS only; every file is checked before the model becomes visible.\n");
    return 2;
}
#endif
