/* Saved plans are routing hints. A green state needs a fresh authenticated
 * answer from EVERY approved donor, for the exact allocation and root. */
#ifndef LUMABRI_RESIDENT_UI_H
#define LUMABRI_RESIDENT_UI_H

#include "src/runtime/lumabri_resident_control.h"

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
    double refreshed = nowd();
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
        int recorded = !home_service_record("prepare", &operation);
        int pending = recorded && operation.state == HOME_SVC_RUNNING;
        if (!probing && !confirm_unload && nowd()-refreshed >= 2) {
            uint8_t old_ids[64][32]; int old_confirmed[64]; size_t old_count = count;
            for (size_t i = 0; i < count; i++) memcpy(old_ids[i], plans[i].allocation, 32);
            memcpy(old_confirmed, confirmed, sizeof confirmed);
            char chosen[64] = "";
            if (count) snprintf(chosen, sizeof chosen, "%s", plans[selected].model);
            size_t new_count = home_resident_library_list(tracker, plans, 64);
            if (!new_count && !home_resident_plan_load(tracker, &plans[0])) new_count = 1;
            count = new_count; selected = 0;
            for (size_t i = 0; i < count; i++) if (!strcmp(chosen, plans[i].model)) selected = (int)i;
            memset(confirmed, 0, sizeof confirmed);
            for (size_t i = 0; i < count; i++) for (size_t j = 0; j < old_count; j++)
                if (!memcmp(plans[i].allocation, old_ids[j], 32)) { confirmed[i] = old_confirmed[j]; break; }
            refreshed = nowd();
        }
        ui_text(5, 5, UI_TEXT, "Approved plans · no implicit loading");
        if (recorded) ui_printf(7, 5, UI_SAND, "Preparation (last recorded): %.100s", operation.detail);
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
