/* Private restart hint, never authority to load or replace an allocation.
 * The authenticated host must still prove the accepted root before text is
 * sent. No token, private key, conversation or weights are stored here. */
#ifndef LUMABRI_RESIDENT_PLAN_H
#define LUMABRI_RESIDENT_PLAN_H

typedef struct {
    char tracker[256], host[64], host_key[65], root[65], model[64];
    uint32_t context, max_new, sessions; /* 0 in older restart hints means one */
    LmbExecutionView execution;
    uint8_t allocation[32], peer_keys[LMB_CLUSTER_MAX_NODES][32];
    double preparation_seconds, prepared_at;
    char content_id[65]; /* source content identity, distinct from the named routing root */
    /* Preparation provenance, not a speed or permission to recreate engines.
     * Numeric ABI/class are learned only from the live authenticated Edge. */
    struct {
        char build_id[65], adapter[32];
        uint32_t goal;
        char hardware[LMB_CLUSTER_MAX_NODES][65], runtime[LMB_CLUSTER_MAX_NODES][65];
        uint32_t threads[LMB_CLUSTER_MAX_NODES];
    } observation;
} LmbResidentPlan;

static int home_resident_digest(const char value[65]) {
    uint8_t bytes[32];
    return memchr(value, 0, 65) && strlen(value) == 64 && !lmb_unhex(bytes, value, 32);
}

static int home_resident_plan_valid(const LmbResidentPlan *p) {
    if (!p || p->execution.count > LMB_CLUSTER_MAX_NODES || p->sessions > LMB_HOST_MAX_SESSIONS) return 0;
    if (!isfinite(p->preparation_seconds) || p->preparation_seconds < 0 || p->preparation_seconds > 86400 ||
        !isfinite(p->prepared_at) || p->prepared_at < 0 || (!!p->prepared_at != !!p->preparation_seconds)) return 0;
    if (lmb_home_nonzero(p->allocation, 32))
        for (uint32_t i = 0; i < p->execution.count; i++) if (!lmb_home_nonzero(p->peer_keys[i], 32)) return 0;
    uint8_t bytes[32];
    if (!memchr(p->content_id, 0, sizeof p->content_id) ||
        (p->content_id[0] && (strlen(p->content_id) != 64 || lmb_unhex(bytes, p->content_id, 32)))) return 0;
    if (p->observation.build_id[0]) {
        if (!p->sessions || !p->content_id[0] || !lmb_home_nonzero(p->allocation, 32) ||
            !home_resident_digest(p->observation.build_id) || p->observation.goal > 1 ||
            !lmb_cal_text(p->observation.adapter, sizeof p->observation.adapter)) return 0;
        for (uint32_t i = 0; i < p->execution.count; i++)
            if (!home_resident_digest(p->observation.hardware[i]) || !home_resident_digest(p->observation.runtime[i]) ||
                !p->observation.threads[i] || p->observation.threads[i] > 256) return 0;
    }
    return lmb_cal_text(p->tracker, sizeof p->tracker) &&
        lmb_cal_text(p->host, sizeof p->host) &&
        lmb_cal_text(p->model, sizeof p->model) &&
        lmb_cal_text(p->host_key, sizeof p->host_key) && strlen(p->host_key) == 64 &&
        !lmb_unhex(bytes, p->host_key, 32) &&
        lmb_cal_text(p->root, sizeof p->root) && strlen(p->root) == 64 &&
        !lmb_unhex(bytes, p->root, 32) &&
        p->context && p->context <= (1u << 20) && p->max_new && p->max_new <= (1u << 20) &&
        lmb_execution_valid(&p->execution);
}

static int home_resident_plan_path(char *path, size_t size) {
    const char *home = getenv("HOME");
    return !home || !*home ? -1 : checked_printf(path, size, "%s/.lumabri/resident-plan", home);
}

static int home_resident_plan_write(const LmbResidentPlan *p, const char *path) {
    if (!home_resident_plan_valid(p)) return -1;
    char temporary[1232];
    if (checked_printf(temporary, sizeof temporary, "%s.XXXXXX", path)) return -1;
    LmbBuf b = {0};
    /* The validated fixed-capacity fields fit this bound. Reserve before
     * packing so allocation failure cannot produce a partial saved plan. */
    if (lmb_buf_reserve(&b, 65536)) return -1;
    int managed = lmb_home_nonzero(p->allocation, 32);
    if (p->sessions && !managed) { free(b.p); return -1; }
    lmb_buf_u32(&b, p->observation.build_id[0] ? 6 : managed ? p->sessions ? 5 : 4 : p->execution.hybrid ? 2 : 1);
    lmb_buf_str(&b, p->tracker); lmb_buf_str(&b, p->host);
    lmb_buf_str(&b, p->host_key); lmb_buf_str(&b, p->root); lmb_buf_str(&b, p->model);
    lmb_buf_u32(&b, p->context); lmb_buf_u32(&b, p->max_new);
    lmb_buf_u32(&b, p->execution.count); lmb_buf_u32(&b, p->execution.layers);
    if (managed || p->execution.hybrid) lmb_buf_u32(&b, p->execution.hybrid);
    for (uint32_t i = 0; i < p->execution.count; i++) {
        const LmbExecutionNode *n = &p->execution.nodes[i];
        lmb_buf_str(&b, n->name); lmb_buf_str(&b, n->address);
        lmb_buf_u32(&b, n->begin); lmb_buf_u32(&b, n->end);
        lmb_buf_u64(&b, n->reserved_bytes); lmb_buf_u32(&b, (uint32_t)n->edge);
    }
    if (managed) {
        lmb_buf_bytes(&b, p->allocation, 32);
        lmb_buf_bytes(&b, p->peer_keys, p->execution.count * 32);
        lmb_cal_put_double(&b, p->preparation_seconds);
        lmb_cal_put_double(&b, p->prepared_at);
        lmb_buf_str(&b, p->content_id);
        if (p->sessions) lmb_buf_u32(&b, p->sessions);
        if (p->observation.build_id[0]) {
            lmb_buf_str(&b, p->observation.build_id); lmb_buf_str(&b, p->observation.adapter);
            lmb_buf_u32(&b, p->observation.goal);
            for (uint32_t i = 0; i < p->execution.count; i++) {
                lmb_buf_str(&b, p->observation.hardware[i]); lmb_buf_str(&b, p->observation.runtime[i]);
                lmb_buf_u32(&b, p->observation.threads[i]);
            }
        }
    }
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
    LmbCur c = {bytes, length, 0}; uint32_t version;
    if (lmb_cur_u32(&c, &version) || version < 1 || version > 6 ||
        lmb_inventory_string(&c, p->tracker, sizeof p->tracker) || strcmp(tracker, p->tracker) ||
        lmb_inventory_string(&c, p->host, sizeof p->host) ||
        lmb_inventory_string(&c, p->host_key, sizeof p->host_key) ||
        lmb_inventory_string(&c, p->root, sizeof p->root) ||
        lmb_inventory_string(&c, p->model, sizeof p->model) ||
        lmb_cur_u32(&c, &p->context) || lmb_cur_u32(&c, &p->max_new) ||
        lmb_cur_u32(&c, &p->execution.count) || lmb_cur_u32(&c, &p->execution.layers) ||
        p->execution.count > LMB_CLUSTER_MAX_NODES) return -1;
    if (version >= 2 && (lmb_cur_u32(&c, &p->execution.hybrid) || p->execution.hybrid > 1 ||
        (version == 2 && !p->execution.hybrid))) return -1;
    for (uint32_t i = 0; i < p->execution.count; i++) {
        LmbExecutionNode *n = &p->execution.nodes[i]; uint32_t edge;
        if (lmb_inventory_string(&c, n->name, sizeof n->name) ||
            lmb_inventory_string(&c, n->address, sizeof n->address) ||
            lmb_cur_u32(&c, &n->begin) || lmb_cur_u32(&c, &n->end) ||
            lmb_cur_u64(&c, &n->reserved_bytes) || lmb_cur_u32(&c, &edge) || edge > 1) return -1;
        n->edge = (int)edge;
    }
    if (version >= 3) {
        size_t size = 32 + p->execution.count * 32;
        if (c.len - c.off < size || (version == 3 && c.len - c.off != size)) return -1;
        memcpy(p->allocation, c.p + c.off, 32); c.off += 32;
        memcpy(p->peer_keys, c.p + c.off, size - 32); c.off += size - 32;
        if (!lmb_home_nonzero(p->allocation, 32)) return -1;
        for (uint32_t i = 0; i < p->execution.count; i++)
            if (!lmb_home_nonzero(p->peer_keys[i], 32)) return -1;
        if (version >= 4 && (lmb_cal_get_double(&c, &p->preparation_seconds) ||
                            lmb_cal_get_double(&c, &p->prepared_at) ||
                            lmb_inventory_string(&c, p->content_id, sizeof p->content_id))) return -1;
        if (version >= 5 && (lmb_cur_u32(&c, &p->sessions) || !p->sessions)) return -1;
        if (version == 6) {
            if (lmb_inventory_string(&c, p->observation.build_id, sizeof p->observation.build_id) ||
                !p->observation.build_id[0] ||
                lmb_inventory_string(&c, p->observation.adapter, sizeof p->observation.adapter) ||
                lmb_cur_u32(&c, &p->observation.goal)) return -1;
            for (uint32_t i = 0; i < p->execution.count; i++)
                if (lmb_inventory_string(&c, p->observation.hardware[i], sizeof p->observation.hardware[i]) ||
                    lmb_inventory_string(&c, p->observation.runtime[i], sizeof p->observation.runtime[i]) ||
                    lmb_cur_u32(&c, &p->observation.threads[i])) return -1;
        }
    }
    return c.off == c.len && home_resident_plan_valid(p) ? 0 : -1;
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

static int home_resident_observation_key(const LmbResidentPlan *p, LmbCalKey *key) {
    memset(key, 0, sizeof *key);
    if (!home_resident_plan_valid(p) || !p->observation.build_id[0]) return -1;
    snprintf(key->model_root, sizeof key->model_root, "%s", p->content_id);
    snprintf(key->adapter, sizeof key->adapter, "%s", p->observation.adapter);
    snprintf(key->build_id, sizeof key->build_id, "%s", p->observation.build_id);
    snprintf(key->plan_kind, sizeof key->plan_kind, "%s", p->execution.hybrid ? "hybrid" : "segment");
    key->goal = p->observation.goal; key->context = p->context; key->sessions = p->sessions;
    key->nodes = p->execution.count;
    for (uint32_t i = 0; i < key->nodes; i++) {
        lmb_hex(key->node_id[i], p->peer_keys[i], 32);
        snprintf(key->node_hardware_id[i], sizeof key->node_hardware_id[i], "%s", p->observation.hardware[i]);
        snprintf(key->node_build_id[i], sizeof key->node_build_id[i], "%s", p->observation.runtime[i]);
        snprintf(key->node_backend[i], sizeof key->node_backend[i], "cpu");
        key->threads[i] = p->observation.threads[i];
        key->layer_begin[i] = p->execution.nodes[i].begin; key->layer_end[i] = p->execution.nodes[i].end;
        if (p->execution.nodes[i].edge) key->edge_node = i;
    }
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
    LmbCalibration resumed = {0}; char records[1200], why[200], binary[1200], bin_dir[1024];
    LmbBinaryDigest self = {0}; uint8_t hash[32]; char build[65];
    int observing = 0;
    if (!g_recording_calibration && p->content_id[0] && !catalog_calibration_dir(records) &&
        (!home_resident_observation_key(p, &resumed.key) || !lmb_cal_load(records, p->content_id, &resumed))) {
        LmbCalKey *k = &resumed.key;
        int same = k->nodes == p->execution.count && k->context == p->context && k->sessions == (p->sessions ? p->sessions : 1) &&
            !strcmp(k->plan_kind, p->execution.hybrid ? "hybrid" : "segment");
        for (uint32_t i = 0; same && i < k->nodes; i++) {
            char peer[65]; lmb_hex(peer, p->peer_keys[i], 32);
            same = !strcmp(peer, k->node_id[i]) && p->execution.nodes[i].begin == k->layer_begin[i] &&
                p->execution.nodes[i].end == k->layer_end[i] && !!p->execution.nodes[i].edge == (i == k->edge_node);
        }
        exe_dir(bin_dir, sizeof bin_dir);
        same = same && !checked_printf(binary, sizeof binary, "%s/lumabri", bin_dir) &&
            !lmb_binary_digest(binary, &self, hash);
        if (same) { lmb_hex(build, hash, 32); same = !strcmp(build, k->build_id); }
        if (same && !catalog_runtime_revalidate_tracker(p->tracker, k, why, sizeof why)) {
            resumed.preparation_seconds = p->preparation_seconds; resumed.prepared_at = p->prepared_at;
            g_recording_calibration = &resumed; g_calibration_directory = records; observing = 1;
        }
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
