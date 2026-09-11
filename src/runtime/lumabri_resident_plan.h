/* Private restart hint, never authority to load or replace an allocation.
 * The authenticated host must still prove the accepted root before text is
 * sent. No token, private key, conversation or weights are stored here. */
#ifndef LUMABRI_RESIDENT_PLAN_H
#define LUMABRI_RESIDENT_PLAN_H

typedef struct {
    char tracker[256], host[64], host_key[65], root[65], model[64];
    uint32_t context, max_new;
    LmbExecutionView execution;
} LmbResidentPlan;

static int home_resident_plan_valid(const LmbResidentPlan *p) {
    uint8_t bytes[32];
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

static int home_resident_plan_save(const LmbResidentPlan *p) {
    if (!home_resident_plan_valid(p)) return -1;
    char path[1200], temporary[1232];
    if (home_resident_plan_path(path, sizeof path) ||
        checked_printf(temporary, sizeof temporary, "%s.XXXXXX", path)) return -1;
    LmbBuf b = {0};
    lmb_buf_u32(&b, 1);
    lmb_buf_str(&b, p->tracker); lmb_buf_str(&b, p->host);
    lmb_buf_str(&b, p->host_key); lmb_buf_str(&b, p->root); lmb_buf_str(&b, p->model);
    lmb_buf_u32(&b, p->context); lmb_buf_u32(&b, p->max_new);
    lmb_buf_u32(&b, p->execution.count); lmb_buf_u32(&b, p->execution.layers);
    for (uint32_t i = 0; i < p->execution.count; i++) {
        const LmbExecutionNode *n = &p->execution.nodes[i];
        lmb_buf_str(&b, n->name); lmb_buf_str(&b, n->address);
        lmb_buf_u32(&b, n->begin); lmb_buf_u32(&b, n->end);
        lmb_buf_u64(&b, n->reserved_bytes); lmb_buf_u32(&b, (uint32_t)n->edge);
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

static int home_resident_plan_load(const char *tracker, LmbResidentPlan *p) {
    memset(p, 0, sizeof *p);
    char path[1200];
    if (home_resident_plan_path(path, sizeof path)) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_size <= 0 || st.st_size > 65536) { close(fd); return -1; }
    uint8_t bytes[65536]; size_t length = (size_t)st.st_size;
    int rc = lmb_read_full(fd, bytes, length); close(fd);
    if (rc) return -1;
    LmbCur c = {bytes, length, 0}; uint32_t version;
    if (lmb_cur_u32(&c, &version) || version != 1 ||
        lmb_inventory_string(&c, p->tracker, sizeof p->tracker) || strcmp(tracker, p->tracker) ||
        lmb_inventory_string(&c, p->host, sizeof p->host) ||
        lmb_inventory_string(&c, p->host_key, sizeof p->host_key) ||
        lmb_inventory_string(&c, p->root, sizeof p->root) ||
        lmb_inventory_string(&c, p->model, sizeof p->model) ||
        lmb_cur_u32(&c, &p->context) || lmb_cur_u32(&c, &p->max_new) ||
        lmb_cur_u32(&c, &p->execution.count) || lmb_cur_u32(&c, &p->execution.layers) ||
        p->execution.count > LMB_CLUSTER_MAX_NODES) return -1;
    for (uint32_t i = 0; i < p->execution.count; i++) {
        LmbExecutionNode *n = &p->execution.nodes[i]; uint32_t edge;
        if (lmb_inventory_string(&c, n->name, sizeof n->name) ||
            lmb_inventory_string(&c, n->address, sizeof n->address) ||
            lmb_cur_u32(&c, &n->begin) || lmb_cur_u32(&c, &n->end) ||
            lmb_cur_u64(&c, &n->reserved_bytes) || lmb_cur_u32(&c, &edge) || edge > 1) return -1;
        n->edge = (int)edge;
    }
    return c.off == c.len && home_resident_plan_valid(p) ? 0 : -1;
}

static int home_resident_plan_chat(const LmbResidentPlan *p) {
    if (!home_resident_plan_valid(p)) return -1;
    char context[20], max_new[20];
    snprintf(context, sizeof context, "%u", p->context);
    snprintf(max_new, sizeof max_new, "%u", p->max_new);
    char *args[] = {"--host", (char *)p->host, "--host-key", (char *)p->host_key,
        "--host-root", (char *)p->root, "--model", (char *)p->model,
        "--tracker", (char *)p->tracker, "--ctx", context, "--max-new", max_new};
    /* A new conversation, not replay of another session's KV. The host
     * serializes admission and resets its state before accepting us. */
    g_execution_view = &p->execution;
    int rc = cmd_chat((int)(sizeof args / sizeof *args), args);
    g_execution_view = NULL;
    if (rc) home_fail("The retained plan is unavailable or changed. Keep its donors sharing, or prepare a new plan from Explore models. No weights were downloaded.");
    return rc;
}
#endif
