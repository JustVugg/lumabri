/* Optional Kubernetes policy adapter. The existing Lumabri service owns
 * preparation, admission, donor consent, draining and durable operations. */
#define _GNU_SOURCE
/* Shared policy validation includes optional header-only signing helpers. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "src/planner/lumabri_elastic_json.h"
#pragma GCC diagnostic pop
#include <curl/curl.h>
#include <stdarg.h>

#define KUBE_LIMIT (2u << 20)
#define KUBE_FINALIZER "lumabri.io/pause-replica-policy"
#define KUBE_GROUP "/apis/lumabri.io/v1alpha1/namespaces/"

typedef struct { char *text; size_t size; LmbJson j; LmbJsonToken *tokens; } Document;
typedef struct {
    const char *server, *ca, *service_token, *api_token, *ns;
    unsigned api_port;
    char collection[256];
} Settings;
static volatile sig_atomic_t stopping;
static void stop_signal(int signal_number) { (void)signal_number; stopping=1; }
static void document_free(Document *d) { free(d->text); free(d->tokens); memset(d,0,sizeof *d); }
static size_t receive(void *data,size_t size,size_t count,void *arg) {
    Document *d=arg;
    if (size && count>SIZE_MAX/size) return 0;
    size_t n=size*count;
    if (n>KUBE_LIMIT-d->size) return 0;
    char *p=realloc(d->text,d->size+n+1); if (!p) return 0;
    d->text=p; memcpy(p+d->size,data,n); d->size+=n; p[d->size]=0; return n;
}
static int decode(Document *d) {
    d->tokens=calloc(65536,sizeof *d->tokens);
    return !d->tokens || lmb_json_parse(&d->j,d->text,d->size,d->tokens,65536);
}
/* Unknown metadata keys belong to Kubernetes. Duplicates of observed keys
 * are refused instead of selecting whichever copy happens to come last. */
static int field(const Document *d,int object,const char *name) {
    if (object<0 || d->tokens[object].kind!=LMB_JSON_OBJECT) return -1;
    int found=-1;
    for (unsigned at=(unsigned)object+1;at<d->tokens[object].next;) {
        char key[128]; unsigned value=at+1;
        if (lmb_api_json_string(&d->j,at,key,sizeof key)) return -1;
        if (!strcmp(key,name)) { if (found>=0) return -1; found=(int)value; }
        at=d->tokens[value].next;
    }
    return found;
}
static int string(const Document *d,int index,char *out,size_t cap) {
    return index<0 || lmb_api_json_string(&d->j,(unsigned)index,out,cap);
}
static int string_field(const Document *d,int object,const char *name,char *out,size_t cap) {
    return string(d,field(d,object,name),out,cap);
}
static int number_field(const Document *d,int object,const char *name,uint32_t *out) {
    int i=field(d,object,name); return i<0 || lmb_json_uint(&d->j,(unsigned)i,out);
}
static int literal(const Document *d,int object,const char *name,unsigned kind) {
    int i=field(d,object,name); return i>=0 && d->tokens[i].kind==kind;
}
static char *slice(const Document *d,int index) {
    if (index<0) return NULL;
    const LmbJsonToken *t=&d->tokens[index];
    return strndup((const char *)d->j.text+t->begin,t->end-t->begin);
}
static int safe_name(const char *s,int dot,size_t limit) {
    size_t n=strlen(s); if (!n || n>limit || *s=='-' || s[n-1]=='-') return 0;
    for (;*s;s++) if (!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='-'||(dot&&*s=='.'))) return 0;
    return 1;
}
static int credential(const char *path,char out[8193],int api) {
    /* Projected Secret/ServiceAccount files rotate through symlinks. Check
     * the opened target; never log bytes or use tokens as command arguments. */
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK); if (fd<0) return -1;
    struct stat st; int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||(st.st_mode&0027)||st.st_size<1||st.st_size>8192;
    ssize_t n=bad ? -1 : read(fd,out,8192); close(fd);
    if (n<1 || n!=st.st_size) return -1;
    while (n && (out[n-1]=='\n'||out[n-1]=='\r')) n--;
    out[n]=0;
    for (ssize_t i=0;i<n;i++) if ((unsigned char)out[i]<=32 || (unsigned char)out[i]>=127) return -1;
    if (api) {
        char *dot=strchr(out,'.');
        if (!dot || dot==out || dot-out>32 || !lmb_preparation_hex(dot+1)) return -1;
        *dot=0; int valid=lmb_api_username(out); *dot='.'; if (!valid) return -1;
    }
    return n ? 0 : -1;
}
static long request(const Settings *s,int kube,const char *method,const char *path,const char *body,Document *out) {
    memset(out,0,sizeof *out);
    char token[8193]={0},authorization[8250],url[2048],origin[100]; long status=0;
    if (credential(kube?s->service_token:s->api_token,token,!kube)) goto done;
    int n=kube ? snprintf(url,sizeof url,"%s%s",s->server,path) :
        snprintf(url,sizeof url,"http://127.0.0.1:%u%s",s->api_port,path);
    if (n<0 || n>=(int)sizeof url) goto done;
    snprintf(authorization,sizeof authorization,"Authorization: Bearer %s",token);
    CURL *c=curl_easy_init(); if (!c) goto done;
    struct curl_slist *headers=NULL;
    headers=curl_slist_append(headers,authorization);
    headers=curl_slist_append(headers,"Accept: application/json");
    headers=curl_slist_append(headers,!strcmp(method,"PATCH")?"Content-Type: application/json-patch+json":"Content-Type: application/json");
    if (!kube) {
        snprintf(origin,sizeof origin,"Origin: http://127.0.0.1:%u",s->api_port);
        headers=curl_slist_append(headers,origin);
    }
    curl_easy_setopt(c,CURLOPT_URL,url);
    curl_easy_setopt(c,CURLOPT_CUSTOMREQUEST,method);
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(c,CURLOPT_PROXY,"");
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,kube?"https":"http");
    curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,2L);
    if (kube) curl_easy_setopt(c,CURLOPT_CAINFO,s->ca);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,3L);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,15L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,out);
    if (body) { curl_easy_setopt(c,CURLOPT_POSTFIELDS,body); curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,(long)strlen(body)); }
    if (curl_easy_perform(c)==CURLE_OK) {
        curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
        if (status>=300 && status<400) status=0;
        if (decode(out)) status=0;
    }
    curl_slist_free_all(headers); curl_easy_cleanup(c);
done:
    lmb_api_wipe(token,sizeof token); lmb_api_wipe(authorization,sizeof authorization);
    return status; /* No automatic write retry. Reconcile against CAS fences. */
}
static void append(char *buf,size_t cap,const char *fmt,...) {
    size_t used=strlen(buf); if (used>=cap) return;
    va_list ap; va_start(ap,fmt); int n=vsnprintf(buf+used,cap-used,fmt,ap); va_end(ap);
    if (n<0 || (size_t)n>=cap-used) buf[0]=0;
}
static int patch(const Settings *s,const char *path,const char *uid,const char *rv,const char *tail) {
    char body[16384]=""; Document response;
    append(body,sizeof body,"[{\"op\":\"test\",\"path\":\"/metadata/uid\",\"value\":\"%s\"},"
        "{\"op\":\"test\",\"path\":\"/metadata/resourceVersion\",\"value\":\"%s\"},%s]",uid,rv,tail);
    if (!*body) return -1;
    long status=request(s,1,"PATCH",path,body,&response); document_free(&response);
    return status==200 ? 0 : -1;
}
static int finalizer(const Settings *s,const Document *d,int meta,const char *path,const char *uid,const char *rv,int remove) {
    int f=field(d,meta,"finalizers"),present=0; char list[2048]="[";
    if (f>=0) {
        if (d->tokens[f].kind!=LMB_JSON_ARRAY || d->tokens[f].children>16) return -1;
        for (unsigned at=(unsigned)f+1;at<d->tokens[f].next;at=d->tokens[at].next) {
            char name[128]; if (string(d,(int)at,name,sizeof name)) return -1;
            for (const char *p=name;*p;p++) if (!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("._-/",*p))) return -1;
            if (!strcmp(name,KUBE_FINALIZER)) { present=1; if (remove) continue; }
            append(list,sizeof list,"%s\"%s\"",strlen(list)>1?",":"",name); if (!*list) return -1;
        }
    }
    if (present==!remove) return 0;
    if (!remove) append(list,sizeof list,"%s\"%s\"",strlen(list)>1?",":"",KUBE_FINALIZER);
    append(list,sizeof list,"]"); if (!*list) return -1;
    char tail[2300]; snprintf(tail,sizeof tail,"{\"op\":\"add\",\"path\":\"/metadata/finalizers\",\"value\":%s}",list);
    return patch(s,path,uid,rv,tail) ? -1 : 1;
}
static void observed(const Settings *s,const Document *d,int object,const char *path,const char *uid,const char *rv,
    uint32_t generation,const char *phase,uint32_t revision,const char *reason) {
    char state[768],tail[850],endpoint[512];
    snprintf(state,sizeof state,"{\"observedGeneration\":%u,\"phase\":\"%s\",\"policyRevision\":%u,\"reason\":\"%s\","
        "\"scope\":\"replica_policy_not_live_model_readiness\"}",generation,phase,revision,reason);
    int old=field(d,object,"status");
    if (old>=0) {
        char p[80],r[128]; uint32_t g=0,v=0;
        if (!string_field(d,old,"phase",p,sizeof p)&&!strcmp(p,phase)&&!string_field(d,old,"reason",r,sizeof r)&&!strcmp(r,reason)&&
            !number_field(d,old,"observedGeneration",&g)&&g==generation&&!number_field(d,old,"policyRevision",&v)&&v==revision) return;
    }
    snprintf(tail,sizeof tail,"{\"op\":\"add\",\"path\":\"/status\",\"value\":%s}",state);
    snprintf(endpoint,sizeof endpoint,"%s/status",path);
    (void)patch(s,endpoint,uid,rv,tail);
}
/* Compare settings only: replica IDs and route revision legitimately change
 * as the service scales. Those runtime changes never grant new authority. */
static int same_settings(const Document *desired,int config,const Document *live,int record,int ignore_enabled) {
    static const char *const keys[]={"id","enabled","min_replicas","max_replicas","cooldown_seconds","idle_seconds",
        "up_rejections","ttft_ms","max_reserved_bytes","source_model","nodes","context","sessions","max_new"};
    for (size_t i=0;i<sizeof keys/sizeof *keys;i++) {
        if (ignore_enabled && !strcmp(keys[i],"enabled")) continue;
        int a=field(desired,config,keys[i]),b=field(live,record,keys[i]);
        if (a<0 || b<0) return 0;
        const LmbJsonToken *x=&desired->tokens[a],*y=&live->tokens[b];
        if (x->kind!=y->kind) return 0;
        if (x->kind==LMB_JSON_STRING) {
            char xs[128],ys[128]; if (string(desired,a,xs,sizeof xs)||string(live,b,ys,sizeof ys)||strcmp(xs,ys)) return 0;
        } else if (x->kind==LMB_JSON_ARRAY) {
            if (x->children!=y->children) return 0;
            unsigned ai=(unsigned)a+1,bi=(unsigned)b+1;
            while (ai<x->next) {
                char xs[128],ys[128]; if (string(desired,(int)ai,xs,sizeof xs)||string(live,(int)bi,ys,sizeof ys)||strcmp(xs,ys)) return 0;
                ai=desired->tokens[ai].next; bi=live->tokens[bi].next;
            }
        } else if (x->end-x->begin!=y->end-y->begin || memcmp(desired->j.text+x->begin,live->j.text+y->begin,x->end-x->begin)) return 0;
    }
    return 1;
}
static void reconcile(const Settings *s,const Document *d,int object) {
    int meta=field(d,object,"metadata"),config=field(d,object,"spec");
    char name[128],uid[128],rv[80],ns[64],path[512],id[65]; uint32_t generation=0;
    if (string_field(d,meta,"name",name,sizeof name)||string_field(d,meta,"uid",uid,sizeof uid)||
        string_field(d,meta,"resourceVersion",rv,sizeof rv)||string_field(d,meta,"namespace",ns,sizeof ns)||strcmp(ns,s->ns)||
        number_field(d,meta,"generation",&generation)||!generation||!safe_name(name,1,127)||!safe_name(uid,0,127)||!safe_name(rv,0,79)) return;
    snprintf(path,sizeof path,"%s/%s",s->collection,name);
    char *body=slice(d,config); LmbElasticPolicy intended; int configure=0;
    if (!body || lmb_elastic_parse(body,strlen(body),&intended,&configure)||configure!=1||!intended.revision||intended.revision>UINT32_MAX-3) {
        observed(s,d,object,path,uid,rv,generation,"Invalid",0,"explicit_existing_policy_and_full_bounded_configuration_required"); free(body); return;
    }
    lmb_hex(id,intended.route.id,32); char expected_name[80];
    snprintf(expected_name,sizeof expected_name,"model-%.32s.%s",id,id+32);
    if (strcmp(name,expected_name)) {
        observed(s,d,object,path,uid,rv,generation,"Invalid",0,"name_must_bind_exact_model_id"); free(body); return;
    }
    int deleting=field(d,meta,"deletionTimestamp")>=0 && !literal(d,meta,"deletionTimestamp",LMB_JSON_NULL);
    if (deleting) {
        int finalizers=field(d,meta,"finalizers"),owned=0;
        if (finalizers>=0 && d->tokens[finalizers].kind==LMB_JSON_ARRAY)
            for (unsigned at=(unsigned)finalizers+1;at<d->tokens[finalizers].next;at=d->tokens[at].next) {
                char name[128];
                if (!string(d,(int)at,name,sizeof name) && !strcmp(name,KUBE_FINALIZER)) owned=1;
            }
        if (!owned) { free(body); return; }
    }
    if (!deleting && finalizer(s,d,meta,path,uid,rv,0)) { free(body); return; }
    Document live; long code=request(s,0,"GET","/api/v1/elastic-policy",NULL,&live); int record=-1;
    int list=code==200?field(&live,0,"policies"):-1;
    if (list>=0 && live.tokens[list].kind==LMB_JSON_ARRAY) for (unsigned at=(unsigned)list+1;at<live.tokens[list].next;at=live.tokens[at].next) {
        char candidate[65];
        if (!string_field(&live,(int)at,"id",candidate,sizeof candidate)&&!strcmp(candidate,id)) {
            if (record>=0) { record=-1; break; } record=(int)at;
        }
    }
    uint32_t revision=0;
    if (record<0 || number_field(&live,record,"revision",&revision)) {
        observed(s,d,object,path,uid,rv,generation,"Unknown",0,"existing_policy_unavailable_no_assumption_of_release"); goto done;
    }
    uint32_t expected=intended.revision;
    if (deleting) {
        /* This CR controls automation. Delete pauses it and retains resident
         * weights/pending ownership. It never kills Pods or conversations. */
        if (!literal(&live,record,"enabled",LMB_JSON_TRUE) &&
            ((revision==expected+1) || (revision==expected+2 && same_settings(d,config,&live,record,1)))) {
            (void)finalizer(s,d,meta,path,uid,rv,1); goto done;
        }
        if (revision!=expected && !(revision==expected+1 && same_settings(d,config,&live,record,0))) {
            observed(s,d,object,path,uid,rv,generation,"Conflict",revision,"policy_changed_deletion_requires_operator_review"); goto done;
        }
        char disable[180]; snprintf(disable,sizeof disable,"{\"action\":\"disable\",\"id\":\"%s\",\"revision\":%u}",id,revision);
        Document reply; (void)request(s,0,"POST","/api/v1/elastic-policy",disable,&reply); document_free(&reply);
        observed(s,d,object,path,uid,rv,generation,"Pausing",revision,"observe_disabled_revision_before_removing_finalizer"); goto done;
    }
    if (revision==expected+1 && same_settings(d,config,&live,record,0)) {
        observed(s,d,object,path,uid,rv,generation,literal(&live,record,"faulted",LMB_JSON_TRUE)?"Faulted":"Applied",revision,
            literal(&live,record,"faulted",LMB_JSON_TRUE)?"inspect_service_pending_operations":"configuration_observed_not_a_readiness_or_speed_claim"); goto done;
    }
    if (revision!=expected) {
        observed(s,d,object,path,uid,rv,generation,"Conflict",revision,"policy_changed_refresh_explicit_revisions_before_retry"); goto done;
    }
    Document reply; code=request(s,0,"POST","/api/v1/elastic-policy",body,&reply); document_free(&reply);
    observed(s,d,object,path,uid,rv,generation,code==200?"Applying":code==409?"Conflict":"Unknown",revision,
        code==200?"observe_exact_settings_after_configuration":code==409?"configuration_or_route_revision_conflict":"write_outcome_unknown_reobserve_before_any_retry");
done:
    document_free(&live); free(body);
}
static int pass(const Settings *s) {
    char path[300]; snprintf(path,sizeof path,"%s?limit=64",s->collection);
    Document list; long status=request(s,1,"GET",path,NULL,&list);
    int items=status==200?field(&list,0,"items"):-1;
    int meta=status==200?field(&list,0,"metadata"):-1; char cursor[128]="";
    int continued=field(&list,meta,"continue");
    if (items<0 || list.tokens[items].kind!=LMB_JSON_ARRAY || list.tokens[items].children>32 ||
        (continued>=0 && (string(&list,continued,cursor,sizeof cursor)||*cursor))) {
        fprintf(stderr,"[lumabri-kube] policy list unavailable, malformed or above 32 policies (HTTP %ld)\n",status);
        document_free(&list); return -1;
    }
    for (unsigned at=(unsigned)items+1;!stopping && at<list.tokens[items].next;at=list.tokens[at].next) reconcile(s,&list,(int)at);
    document_free(&list); return 0;
}
static int server_valid(const char *url) {
    CURLU *u=curl_url(); if (!u) return 0;
    char *scheme=NULL,*host=NULL,*part=NULL; int valid=0;
    if (curl_url_set(u,CURLUPART_URL,url,0)||curl_url_get(u,CURLUPART_SCHEME,&scheme,0)||strcmp(scheme,"https")||
        curl_url_get(u,CURLUPART_HOST,&host,0)||!*host) goto done;
    const CURLUPart forbidden[]={CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_QUERY,CURLUPART_FRAGMENT};
    for (size_t i=0;i<sizeof forbidden/sizeof *forbidden;i++) {
        if (!curl_url_get(u,forbidden[i],&part,0)) { curl_free(part); part=NULL; goto done; }
    }
    if (!curl_url_get(u,CURLUPART_PATH,&part,0)&&strcmp(part,"/")) goto done;
    valid=1;
done:
    curl_free(part); curl_free(host); curl_free(scheme); curl_url_cleanup(u); return valid;
}
int main(int argc,char **argv) {
    Settings s={.api_port=47380}; int once=0;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--help")) {
            puts("lumabri-kube --server https://HOST:PORT --ca FILE --service-token FILE --api-token FILE --namespace NAME [--api-port PORT] [--once]"); return 0;
        }
        if (!strcmp(argv[i],"--once")) once=1;
        else if (i+1<argc && !strcmp(argv[i],"--server")) s.server=argv[++i];
        else if (i+1<argc && !strcmp(argv[i],"--ca")) s.ca=argv[++i];
        else if (i+1<argc && !strcmp(argv[i],"--service-token")) s.service_token=argv[++i];
        else if (i+1<argc && !strcmp(argv[i],"--api-token")) s.api_token=argv[++i];
        else if (i+1<argc && !strcmp(argv[i],"--namespace")) s.ns=argv[++i];
        else if (i+1<argc && !strcmp(argv[i],"--api-port")) {
            char *end; unsigned long p=strtoul(argv[++i],&end,10);
            if (!*argv[i]||*end||!p||p>65535) return 2;
            s.api_port=(unsigned)p;
        } else { fputs("lumabri-kube --server https://HOST:PORT --ca FILE --service-token FILE --api-token FILE --namespace NAME [--api-port PORT] [--once]\n",stderr); return 2; }
    }
    if (!s.server||!s.ca||!s.service_token||!s.api_token||!s.ns||!safe_name(s.ns,0,63)||strlen(s.server)>512||
        s.server[strlen(s.server)-1]=='/'||curl_global_init(CURL_GLOBAL_DEFAULT)||!server_valid(s.server)) return 2;
    snprintf(s.collection,sizeof s.collection,KUBE_GROUP "%s/replicapolicies",s.ns);
    signal(SIGPIPE,SIG_IGN); signal(SIGTERM,stop_signal); signal(SIGINT,stop_signal);
    int rc=0;
    do {
        rc=pass(&s);
        if (!once) for (unsigned i=0;i<5&&!stopping;i++) sleep(1);
    } while (!once&&!stopping);
    curl_global_cleanup(); return rc?1:0;
}
