/* Native private storage and live provenance for the shared turn observer.
 * Stored values contain no conversation text. Missing evidence is not a rate. */
#ifndef LMB_CALIBRATION_WINDOWS_H
#define LMB_CALIBRATION_WINDOWS_H
#include "src/runtime/lumabri_resident_plan_windows.h"
#include "src/planner/lumabri_catalogue_data.h"
#include "src/planner/lumabri_calibration_key.h"
#include "src/planner/lumabri_turn_observation.h"
#include "src/runtime/lumabri_host_client.h"

static int catalog_build_windows(char hex[65]) {
    static LmbSourceStat cached;static char identity[65];
    wchar_t wide[2048];LmbSourceStat before,after;
    DWORD n=GetModuleFileNameW(NULL,wide,2048);hex[0]=0;
    if (!n || n>=2048) return -1;
    /* Hash only our loaded executable, which may live on a development UNC
     * share. The stricter local-volume rule still applies to checkpoint roots. */
    HANDLE file=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (file==INVALID_HANDLE_VALUE) return -1;
    if (lmb_source_info(file,&before) || !before.regular) {CloseHandle(file);return -1;}
    if (identity[0] && lmb_source_same(&cached,&before)) {strcpy(hex,identity);CloseHandle(file);return 0;}
    LmbSha sha;uint8_t buffer[65536],digest[32];uint64_t at=0;lmb_sha_init(&sha);
    while (at<before.size) {
        DWORD take=before.size-at<sizeof buffer ? (DWORD)(before.size-at) : sizeof buffer,got=0;
        if (!ReadFile(file,buffer,take,&got,NULL) || !got) break;
        lmb_sha_update(&sha,buffer,got);at+=got;
    }
    int ok=at==before.size && !lmb_source_info(file,&after) && lmb_source_same(&before,&after);
    CloseHandle(file);if (!ok) return -1;
    lmb_sha_final(&sha,digest);lmb_hex(identity,digest,32);cached=before;strcpy(hex,identity);return 0;
}
static int catalog_calibration_path_windows(const char *root,char path[1400]) {
    char directory[1200];uint8_t digest[32];
    if (!root || strlen(root)!=64 || lmb_unhex(digest,root,32) ||
        lmb_private_default_path(directory,sizeof directory,"calibrations") || lmb_private_mkdir(directory)) return -1;
    int n=snprintf(path,1400,"%s\\%s.cal",directory,root);return n<0 || n>=1400 ? -1 : 0;
}
static int catalog_calibration_load_windows(const char *root,LmbCalibration *record) {
    char path[1400];uint8_t data[LMB_CAL_RECORD_MAX];size_t size;memset(record,0,sizeof *record);
    if (catalog_calibration_path_windows(root,path) || lmb_private_blob_read(path,data,sizeof data,&size) ||
        lmb_cal_decode(data,size,record) || strcmp(record->key.model_root,root)) {
        memset(record,0,sizeof *record);return -1;
    }
    return 0;
}
static int catalog_calibration_save_windows(const LmbCalibration *record) {
    char path[1400];LmbBuf b={0};
    int rc=catalog_calibration_path_windows(record->key.model_root,path) || lmb_cal_encode(record,&b) ||
        lmb_private_blob_write(path,b.p,b.len);free(b.p);return rc ? -1 : 0;
}
static int catalog_workload_windows(const LmbResidentPlan *plan,LmbCalKey *key) {
    LmbMachineReport reports[LMB_INVENTORY_MAX];uint32_t count=0;char why[200];
    if (lmb_inventory_fetch(plan->tracker,reports,&count) ||
        catalog_runtime_match(key,reports,count,why,sizeof why)) return -1;
    for (uint32_t j=0;j<key->nodes;j++) {
        int found=0;
        for (uint32_t i=0;i<count;i++) {
            char peer[65];lmb_hex(peer,reports[i].identity,32);
            if (strcmp(peer,key->node_id[j])) continue;
            LmbWorkloadFacts w=reports[i].workload;
            if (!w.known || !w.allocations || w.reserved_bytes<plan->execution.nodes[j].reserved_bytes) return -1;
            w.active=w.queued=0;key->workload[j]=w;found=1;break;
        }
        if (!found) return -1;
    }
    return 0;
}
typedef struct {
    const LmbResidentPlan *plan;
    LmbCalibration seed;
    LmbCalKey before;
    int observed,saved;
    uint32_t source;
} LmbWindowsObservation;
static int catalog_observe_begin_windows(LmbWindowsObservation *o,const LmbHostGreeting *g) {
    o->observed=o->saved=0;char build[65];
    if (!o->plan || home_resident_observation_key(o->plan,&o->before) ||
        catalog_build_windows(build) || strcmp(build,o->before.build_id) ||
        strcmp(g->backend,"cpu") || !g->numeric_abi || !g->numeric_class[0] ||
        catalog_workload_windows(o->plan,&o->before)) return -1;
    o->before.adapter_abi=g->numeric_abi;
    snprintf(o->before.numeric_class,sizeof o->before.numeric_class,"%s",g->numeric_class);
    if (!lmb_cal_key_valid(&o->before)) return -1;
    if (catalog_calibration_load_windows(o->plan->content_id,&o->seed)) memset(&o->seed,0,sizeof o->seed);
    o->seed.preparation_seconds=o->plan->preparation_seconds;o->seed.prepared_at=o->plan->prepared_at;
    o->observed=1;return 0;
}
static int catalog_observe_end_windows(LmbWindowsObservation *o,const LmbHostGreeting *g,const char *stat,double ttft) {
    if (!o || !o->observed) return -1;
    o->observed=0;LmbCalKey after=o->before;LmbCalibration record;
    if (catalog_workload_windows(o->plan,&after) || !lmb_cal_matches(&o->before,&after) ||
        lmb_turn_observation(&record,&o->seed,&o->before,stat,g->numeric_abi,g->numeric_class,
            ttft,(double)time(NULL),o->source) || catalog_calibration_save_windows(&record)) return -1;
    o->seed=record;o->saved=1;return 0;
}
#endif
