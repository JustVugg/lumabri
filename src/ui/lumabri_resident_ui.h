/* Saved plans are routing hints. A green state needs a fresh authenticated
 * answer from EVERY approved donor, for the exact allocation and root. */
#ifndef LUMABRI_RESIDENT_UI_H
#define LUMABRI_RESIDENT_UI_H

static int home_resident_peer(const LmbResidentPlan *p, uint32_t i, int release) {
    if (!home_resident_plan_valid(p) || !lmb_home_nonzero(p->allocation, 32) ||
        i >= p->execution.count || !lmb_home_nonzero(p->peer_keys[i], 32)) return -1;
    int fd = lmb_connect_ms_io(p->execution.nodes[i].address, 600, 800);
    if (fd < 0) return -1;
    uint8_t body[64]; memcpy(body, p->allocation, 32);
    int bad = lmb_unhex(body + 32, p->root, 32) || !lmb_secure_peer_matches(fd, p->peer_keys[i]) || lmb_auth(fd);
    LmbMsg msg = {0};
    if (!bad) bad = lmb_send(fd, release ? LMB_HOME_RELEASE : LMB_HOME_QUERY, body, sizeof body, NULL, 0) || lmb_recv(fd, &msg);
    lmb_close(fd);
    LmbCur c = {msg.body, msg.body_len, 0}; uint32_t version, phase, segment, host;
    char reason[160];
    if (!bad) bad = msg.op != LMB_HOME_STATUS || msg.pay_len || lmb_cur_u32(&c, &version) ||
        version != LMB_HOME_VERSION || c.len - c.off < 32;
    if (!bad) { bad = memcmp(c.p + c.off, p->allocation, 32); c.off += 32; }
    if (!bad) bad = lmb_cur_u32(&c, &phase) || lmb_cur_u32(&c, &segment) ||
        lmb_cur_u32(&c, &host) || lmb_inventory_string(&c, reason, sizeof reason) || c.off != c.len;
    if (!bad) bad = release ? phase != LMB_HOME_CLOSED || segment || host :
        !segment || (p->execution.nodes[i].edge ? phase != LMB_HOME_READY || !host : phase != LMB_HOME_SEGMENT_READY);
    lmb_msg_free(&msg); return bad ? -1 : 0;
}

typedef struct {
    LmbResidentPlan plan;
    _Atomic int done, cancel;
    int ok, release;
} HomeResidentProbe;

static void *home_resident_probe(void *arg) {
    HomeResidentProbe *p = arg; p->ok = 1;
    for (uint32_t i = 0; i < p->plan.execution.count; i++) {
        if (atomic_load(&p->cancel)) { p->ok = 0; break; }
        if (home_resident_peer(&p->plan, i, p->release)) p->ok = 0;
    }
    atomic_store(&p->done, 1); return NULL;
}

static int home_resident_library_ui(const char *tracker) {
    LmbResidentPlan *plans = calloc(64, sizeof *plans);
    if (!plans) return 1;
    size_t count = home_resident_library_list(tracker, plans, 64);
    if (!count && !home_resident_plan_load(tracker, &plans[0])) count = 1;
    int selected = 0, result = 0, confirmed[64] = {0}, probing = 0, probe_index = -1, confirm_unload = 0;
    HomeResidentProbe probe = {0}; pthread_t thread;
    HomeTerminal term; home_terminal_begin(&term);
    char notice[200] = "Only live, authenticated donor replies confirm a resident model.";
    while (!g_stopping) {
        if (probing && atomic_load(&probe.done)) {
            pthread_join(thread, NULL); probing = 0;
            confirmed[probe_index] = probe.release ? -1 : probe.ok ? 1 : -1;
            snprintf(notice, sizeof notice, "%s", probe.release ? probe.ok ?
                "All donors confirmed release. Their sharing services remain available." :
                "Some releases were not confirmed. The saved plan remains; check the donors." : probe.ok ?
                "Every donor confirmed this exact approved allocation is ready." :
                "Not all donors confirmed readiness. No weights were reloaded.");
        }
        ui_begin("resident models");
        HomeServiceSnapshot operation;
        int pending = !home_service_record("prepare", &operation) && operation.state == HOME_SVC_RUNNING;
        ui_text(5, 5, UI_TEXT, "Approved plans · no implicit loading");
        if (pending) ui_printf(7, 5, UI_SAND, "Preparation (last recorded): %.80s", operation.detail);
        if (!count) ui_text(10, 5, UI_MUTED, "No saved plans for this household. Prepare one from Explore models.");
        int rows = (ui_h - 16) / 3; if (rows < 1) rows = 1;
        int first = selected >= rows ? selected - rows + 1 : 0;
        for (size_t i = (size_t)first; i < count && i < (size_t)(first + rows); i++) {
            uint64_t ram = 0;
            for (uint32_t j = 0; j < plans[i].execution.count; j++) ram += plans[i].execution.nodes[j].reserved_bytes;
            char detail[180];
            snprintf(detail, sizeof detail, "%s · %u computers · %.2f GB reserved by the plan", confirmed[i] > 0 ?
                "Ready at last check" : confirmed[i] < 0 ? "Unavailable / unconfirmed" : "Saved · not checked", plans[i].execution.count, ram / 1e9);
            ui_item(10 + ((int)i - first) * 3, selected == (int)i, plans[i].model, detail);
        }
        ui_footer(confirm_unload ? "Release this model on ALL its approved donors? Active chats will stop. Enter confirms; Esc cancels." : notice,
            probing ? "Checking authenticated donors…   Esc back" :
            "↑ ↓ choose   r verify   Enter chat   x unload   c cancel background preparation   Esc back");
        ui_present();
        int key = home_key();
        if (!probing && key == 'r' && !count) count = home_resident_library_list(tracker, plans, 64);
        if (key == 3 || (key == 27 && !confirm_unload)) break;
        if (key == 27) { confirm_unload = 0; continue; }
        if (key == 'c' && pending) {
            HomeServiceSnapshot live;
            if (!home_service_query("prepare", HOME_SVC_STATUS, NULL, &live) &&
                !memcmp(live.instance, operation.instance, 32) &&
                !home_service_query("prepare", HOME_SVC_CANCEL, &live, &live))
                snprintf(notice, sizeof notice, "Cancellation requested; incomplete allocations will be released.");
            else snprintf(notice, sizeof notice, "Cancellation not confirmed. Check service status before retrying.");
        }
        if (!probing && count) {
            if (!confirm_unload && key == 1001) selected = (selected + (int)count - 1) % (int)count;
            if (!confirm_unload && key == 1002) selected = (selected + 1) % (int)count;
            if (key == 'x') { confirm_unload = 1; continue; }
            if (key == 'r' || ((key == '\r' || key == '\n') && confirm_unload)) {
                memset(&probe, 0, sizeof probe); probe.plan = plans[selected]; probe.release = confirm_unload;
                confirm_unload = 0; probe_index = selected;
                if (!pthread_create(&thread, NULL, home_resident_probe, &probe)) probing = 1;
            } else if (key == '\r' || key == '\n') {
                home_terminal_end(&term);
                result = home_resident_plan_chat(&plans[selected]);
                home_terminal_begin(&term); confirmed[selected] = 0;
                snprintf(notice, sizeof notice, "Conversation closed. Allocations were not unloaded.");
            }
        }
        (void)poll(NULL, 0, 80);
    }
    if (probing) { atomic_store(&probe.cancel, 1); pthread_join(thread, NULL); }
    home_terminal_end(&term); free(plans); return result;
}
#endif
