/* Private restart hint, never authority to load or replace an allocation.
 * The authenticated host must still prove the accepted root before text is
 * sent. No token, private key, conversation or weights are stored here. */
#ifndef LUMABRI_RESIDENT_PLAN_H
#define LUMABRI_RESIDENT_PLAN_H

#include "src/runtime/lumabri_resident_plan_codec.h"

static int home_resident_plan_path(char *path, size_t size) {
    const char *home = getenv("HOME");
    return !home || !*home ? -1 : checked_printf(path, size, "%s/.lumabri/resident-plan", home);
}

static int home_resident_plan_write(const LmbResidentPlan *p, const char *path) {
    char temporary[1232];LmbBuf b={0};
    if (checked_printf(temporary,sizeof temporary,"%s.XXXXXX",path) || home_resident_plan_pack(p,&b)) return -1;
    int fd = mkstemp(temporary), rc = -1;
    if (fd >= 0) {
        rc = fchmod(fd, 0600);
        size_t written = 0;
        while (!rc && written < b.len) {
            ssize_t n = write(fd, b.p + written, b.len - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) rc = -1;
            else written += (size_t)n;
        }
        if (!rc && fsync(fd)) rc = -1;
        if (close(fd)) rc = -1;
        if (!rc && rename(temporary, path)) rc = -1;
        if (rc) unlink(temporary);
    }
    free(b.p); return rc;
}

static int home_resident_plan_save(const LmbResidentPlan *p) {
    char path[1200];
    return home_resident_plan_path(path, sizeof path) ? -1 : home_resident_plan_write(p, path);
}

static int home_resident_plan_read(const char *path, const char *tracker, LmbResidentPlan *p) {
    memset(p, 0, sizeof *p);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_size <= 0 || st.st_size > 65536) { close(fd); return -1; }
    uint8_t bytes[65536]; size_t length = (size_t)st.st_size;
    int rc = lmb_read_full(fd, bytes, length); close(fd);
    if (rc) return -1;
    return home_resident_plan_unpack(bytes,length,tracker,p);
}

static int home_resident_plan_load(const char *tracker, LmbResidentPlan *p) {
    char path[1200];
    return home_resident_plan_path(path, sizeof path) ? -1 : home_resident_plan_read(path, tracker, p);
}

static int home_resident_library_directory(char *path, size_t cap) {
    const char *base = getenv("HOME"); struct stat st;
    if (!base || checked_printf(path, cap, "%s/.lumabri/resident-plans", base)) return -1;
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    return lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) ? -1 : 0;
}

static int home_resident_library_path(const LmbResidentPlan *p, char *path, size_t cap) {
    char dir[1200], id[65]; uint8_t hash[64]; LmbBuf key = {0};
    if (home_resident_library_directory(dir, sizeof dir)) return -1;
    int rc = lmb_buf_str(&key, p->tracker) || lmb_buf_str(&key, p->model) ||
        lmb_buf_str(&key, p->host_key) || lmb_buf_bytes(&key, p->allocation, 32);
    if (!rc) { lmb_sha512(key.p, key.len, hash); lmb_hex(id, hash, 32); }
    free(key.p);
    return rc || checked_printf(path, cap, "%s/%s.plan", dir, id) ? -1 : 0;
}

static int home_resident_library_save(const LmbResidentPlan *p) {
    char path[1400];
    return home_resident_library_path(p, path, sizeof path) ? -1 : home_resident_plan_write(p, path);
}

static size_t home_resident_library_list(const char *tracker, LmbResidentPlan *plans, size_t cap) {
    char dir[1200]; size_t n = 0;
    if (home_resident_library_directory(dir, sizeof dir)) return 0;
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *ent;
    while (n < cap && (ent = readdir(d))) {
        uint8_t hash[32]; char hex[65], path[1400];
        if (strlen(ent->d_name) != 69 || strcmp(ent->d_name + 64, ".plan")) continue;
        memcpy(hex, ent->d_name, 64); hex[64] = 0;
        if (lmb_unhex(hash, hex, 32) || checked_printf(path, sizeof path, "%s/%s", dir, ent->d_name)) continue;
        if (!home_resident_plan_read(path, tracker, &plans[n])) n++;
    }
    closedir(d); return n;
}

/* Management resolves an exact allocation, not a truncated UI page. Keep
 * bounded memory even with years of retained plans; duplicate identities are
 * ambiguous and must not authorize a retirement of either record. */
static int home_resident_library_find(const char *tracker,const uint8_t allocation[32],LmbResidentPlan *out) {
    char dir[1200];
    if (home_resident_library_directory(dir,sizeof dir)) return -1;
    DIR *d=opendir(dir); if (!d) return -1;
    LmbResidentPlan *candidate=calloc(1,sizeof *candidate);
    int found=0,bad=!candidate; struct dirent *entry;
    while (!bad) {
        errno=0; entry=readdir(d);
        if (!entry) { bad=errno!=0; break; }
        if (strlen(entry->d_name)!=69 || strcmp(entry->d_name+64,".plan")) continue;
        char hex[65],path[1400]; uint8_t hash[32];
        memcpy(hex,entry->d_name,64); hex[64]=0;
        if (lmb_unhex(hash,hex,32) || checked_printf(path,sizeof path,"%s/%s",dir,entry->d_name)) continue;
        if (home_resident_plan_read(path,tracker,candidate)) continue;
        if (memcmp(candidate->allocation,allocation,32)) continue;
        if (found++) { bad=1; break; }
        *out=*candidate;
    }
    free(candidate); closedir(d);
    return !bad && found==1 ? 0 : -1;
}

/* Shared provenance preparation, without waiting for an inventory heartbeat.
 * Callers must still validate the actual runtime/workload before observing. */
static int home_resident_observation_seed(const LmbResidentPlan *p, LmbCalibration *resumed, char records[1200]) {
    char binary[1200], bin_dir[1024], build[65]; LmbBinaryDigest self={0}; uint8_t hash[32];
    memset(resumed,0,sizeof *resumed);
    if (!p->content_id[0] || catalog_calibration_dir(records) ||
        (home_resident_observation_key(p,&resumed->key) && lmb_cal_load(records,p->content_id,resumed))) return -1;
    LmbCalKey *k=&resumed->key;
    int same=k->nodes==p->execution.count && k->context==p->context && k->sessions==(p->sessions ? p->sessions : 1) &&
        !strcmp(k->plan_kind,p->execution.hybrid ? "hybrid" : "segment");
    for (uint32_t i=0;same && i<k->nodes;i++) {
        char peer[65]; lmb_hex(peer,p->peer_keys[i],32);
        same=!strcmp(peer,k->node_id[i]) && p->execution.nodes[i].begin==k->layer_begin[i] &&
            p->execution.nodes[i].end==k->layer_end[i] && !!p->execution.nodes[i].edge==(i==k->edge_node);
    }
    exe_dir(bin_dir,sizeof bin_dir);
    same=same && !checked_printf(binary,sizeof binary,"%s/lumabri",bin_dir) && !lmb_binary_digest(binary,&self,hash);
    if (same) { lmb_hex(build,hash,32); same=!strcmp(build,k->build_id); }
    if (!same) return -1;
    resumed->preparation_seconds=p->preparation_seconds; resumed->prepared_at=p->prepared_at;
    return 0;
}

static int home_resident_plan_chat_mode(const LmbResidentPlan *p, int calibrate) {
    if (!home_resident_plan_valid(p)) return -1;
    char context[20], max_new[20];
    snprintf(context, sizeof context, "%u", p->context);
    snprintf(max_new, sizeof max_new, "%u", p->max_new);
    char *args[] = {"--host", (char *)p->host, "--host-key", (char *)p->host_key,
        "--host-root", (char *)p->root, "--model", (char *)p->model,
        "--tracker", (char *)p->tracker, "--ctx", context, "--max-new", max_new, "--calibrate"};
    /* A new conversation, not replay of another session's KV. The host
     * serializes admission and resets its state before accepting us. */
    g_execution_view = &p->execution;
    LmbCalibration resumed = {0}; char records[1200], why[200];
    int observing = 0;
    if (!g_recording_calibration && !home_resident_observation_seed(p,&resumed,records) &&
        !catalog_runtime_revalidate_tracker(p->tracker,&resumed.key,why,sizeof why)) {
        g_recording_calibration=&resumed; g_calibration_directory=records; observing=1;
    }
    int rc = cmd_chat((int)(sizeof args / sizeof *args) - !calibrate, args);
    if (observing) { g_recording_calibration = NULL; g_calibration_directory = NULL; }
    g_execution_view = NULL;
    if (rc == CHAT_REQUEST_FAILED)
        home_fail("The engine rejected the request. Check its message above (for example, the conversation may exceed the context limit). No incomplete reply or speed was saved; resident weights were not released.");
    else if (rc) home_fail("The retained plan is unavailable or changed. Keep its donors sharing, or prepare a new plan from Explore models. No weights were downloaded.");
    return rc;
}

static int home_resident_plan_chat(const LmbResidentPlan *p) { return home_resident_plan_chat_mode(p, 0); }
#endif
