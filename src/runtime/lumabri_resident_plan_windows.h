/* Same resident-plan codec as Unix, stored under this Windows user's ACL. */
#ifndef LMB_RESIDENT_PLAN_WINDOWS_H
#define LMB_RESIDENT_PLAN_WINDOWS_H
#include "src/runtime/lumabri_resident_plan_codec.h"
#include "src/runtime/lumabri_private_blob_windows.h"
#include "src/runtime/lumabri_source_file.h"
static int home_resident_library_directory(char *path,size_t cap) {
    return lmb_private_default_path(path,cap,"resident-plans") || lmb_private_mkdir(path) ? -1 : 0;
}
static int home_resident_library_path(const LmbResidentPlan *p,char *path,size_t cap) {
    char dir[1200],id[65];uint8_t hash[64];LmbBuf key={0};
    if (home_resident_library_directory(dir,sizeof dir)) return -1;
    int rc=lmb_buf_str(&key,p->tracker)||lmb_buf_str(&key,p->model)||
        lmb_buf_str(&key,p->host_key)||lmb_buf_bytes(&key,p->allocation,32);
    if (!rc) {lmb_sha512(key.p,key.len,hash);lmb_hex(id,hash,32);}free(key.p);
    int n=rc ? -1 : snprintf(path,cap,"%s\\%s.plan",dir,id);
    return n<0 || (size_t)n>=cap ? -1 : 0;
}
static int home_resident_plan_write(const LmbResidentPlan *p,const char *path) {
    LmbBuf b={0};int rc=home_resident_plan_pack(p,&b) || lmb_private_blob_write(path,b.p,b.len);
    free(b.p);return rc ? -1 : 0;
}
static int home_resident_plan_read(const char *path,const char *tracker,LmbResidentPlan *p) {
    uint8_t bytes[65536];size_t length;
    return lmb_private_blob_read(path,bytes,sizeof bytes,&length) ||
        home_resident_plan_unpack(bytes,length,tracker,p) ? -1 : 0;
}
static int home_resident_library_save(const LmbResidentPlan *p) {
    char path[1400];return home_resident_library_path(p,path,sizeof path) ? -1 : home_resident_plan_write(p,path);
}
typedef struct {const char *tracker,*directory;LmbResidentPlan *plans;size_t count,cap;} LmbResidentList;
static int home_resident_list_entry(const char *name,void *context) {
    LmbResidentList *l=context;char hex[65],path[1400];uint8_t hash[32];
    if (l->count==l->cap || strlen(name)!=69 || strcmp(name+64,".plan")) return 0;
    memcpy(hex,name,64);hex[64]=0;
    if (lmb_unhex(hash,hex,32)) return 0;
    int n=snprintf(path,sizeof path,"%s\\%s",l->directory,name);
    if (n>=0 && (size_t)n<sizeof path && !home_resident_plan_read(path,l->tracker,&l->plans[l->count])) l->count++;
    return 0;
}
static size_t home_resident_library_list(const char *tracker,LmbResidentPlan *plans,size_t cap) {
    char dir[1200];if (home_resident_library_directory(dir,sizeof dir)) return 0;
    LmbResidentList list={tracker,dir,plans,0,cap};
    return lmb_source_list(dir,home_resident_list_entry,&list) ? 0 : list.count;
}
typedef struct {const char *tracker,*directory;const uint8_t *id;LmbResidentPlan *plan;unsigned found;} LmbResidentFind;
static int home_resident_find_entry(const char *name,void *context) {
    LmbResidentFind *f=context;char path[1400];LmbResidentPlan p;
    if (strlen(name)!=69 || strcmp(name+64,".plan")) return 0;
    int n=snprintf(path,sizeof path,"%s\\%s",f->directory,name);
    if (n<0 || (size_t)n>=sizeof path || home_resident_plan_read(path,f->tracker,&p) || memcmp(p.allocation,f->id,32)) return 0;
    if (!f->found) *f->plan=p;
    f->found++;return f->found>1 ? -1 : 0;
}
static int home_resident_library_find(const char *tracker,const uint8_t id[32],LmbResidentPlan *plan) {
    char dir[1200];if (home_resident_library_directory(dir,sizeof dir)) return -1;
    LmbResidentFind find={tracker,dir,id,plan,0};
    return lmb_source_list(dir,home_resident_find_entry,&find) || find.found!=1 ? -1 : 0;
}
#endif
