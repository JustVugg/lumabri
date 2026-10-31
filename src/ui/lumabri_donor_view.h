/* Shared donor presentation. Reads a value snapshot; never owns a service,
 * allocation, network socket or terminal input loop. */
#ifndef LMB_DONOR_VIEW_H
#define LMB_DONOR_VIEW_H
#include "lumabri_visual.h"
#include "lumabri_home.h"
typedef struct {
    LmbHomeTransaction transaction;
    unsigned thread_capacity,display_models,display_active,display_queued;
    uint64_t display_reserved;
    int retained,persistent,resident,repeat_supported,display_compute;
    const char *log;
} LmbDonorView;
static void lmb_donor_screen(const LmbDonorView *d, const char *name, uint64_t ram, int clear, int choice) {
    const LmbHomeTransaction *t = &d->transaction;
    if (clear) {
        ui_begin("share resources");
        int repeat=d->repeat_supported && d->persistent && d->resident && !t->offer.hybrid_role && ui_h>=34;
        ui_printf(5, 5, UI_TEXT, "%s · up to %.1f GB RAM · CPU execution", name, ram / 1e9);
        ui_text(7, 5, UI_SAND, d->retained ? "Weights retained in RAM; ready for another chat" : lmb_home_phase_name(t->phase));
        if (d->persistent && d->display_models > 1 && d->display_compute)
            ui_printf(8, 5, UI_MUTED, "%u models · %.2f GB reserved · CPU FIFO: %u active, %u waiting · Tab switches model",
                      d->display_models, d->display_reserved / 1e9, d->display_active, d->display_queued);
        else if (d->persistent && d->display_models > 1)
            ui_printf(8, 5, UI_MUTED, "%u models · %.2f GB reserved · Compute admission unknown · Tab switches model",
                      d->display_models, d->display_reserved / 1e9);
        if (t->phase != LMB_HOME_IDLE) {
            char who[65]; lmb_hex(who, t->offer.requester, 32);
            ui_printf(10, 5, UI_TEXT, "Model: %s (%s)", t->offer.model, t->offer.model_type);
            ui_printf(12, 5, UI_MUTED, "Requester identity: %.24s…", who);
            ui_printf(14, 5, UI_TEXT, "Layers %u–%u of %u · %.2f GB RAM · %.2f GB %s",
                t->offer.begin, t->offer.end - 1, t->offer.layers, t->offer.ram_bytes / 1e9,
                t->offer.disk_bytes / 1e9, d->resident ? "metadata headroom" : "disk headroom");
            ui_printf(16, 5, UI_MUTED, "%u context · %u session slot(s) · %u threads · up to %u new tokens per turn", t->offer.context,
                      t->offer.sessions ? t->offer.sessions : 1,
                      t->offer.threads < d->thread_capacity ? t->offer.threads : d->thread_capacity, t->offer.max_new);
            ui_text(18, 5, UI_MUTED, t->offer.hybrid_role == LMB_HYBRID_ACCELERATOR ?
                "Expert accelerator: approved layers only; weights remain in RAM." : t->offer.runs_edge ?
                "This computer hosts chat and receives the conversation text." :
                "This computer processes activations and keeps state for its layers.");
        } else {
            ui_text(11, 5, UI_TEXT, "Visible to your household. Waiting for a request.");
            ui_printf(14, 5, UI_MUTED, "Runtime supports up to %u thread(s). Nothing loads without approval.", d->thread_capacity);
        }
        int y = ui_h >= 37 ? 23 : 20;
        if (t->phase == LMB_HOME_PENDING) {
            ui_item(y, choice == 0, "Accept this request", d->resident ?
                "Keep these weights in RAM until you stop sharing." : "Reserve only the displayed resources for this plan.");
            ui_item(y + 3, choice == 1, "Decline", "No weights or model state will be loaded.");
            if (repeat) ui_item(y + 6, choice == 2, "Allow repeat loads for 1 hour",
                "Same requester, checkpoint and layers; one allocation, same limits. Press r to revoke.");
        } else ui_item(y, 0, d->persistent ? "Sharing continues in the background" : "Keep this window open to share", d->retained ?
            d->repeat_supported ? "Weights stay in RAM. Additional allocations need free capacity and owner approval or a bounded policy." :
            "Weights stay in RAM. Additional allocations need free capacity and your approval." : d->persistent ?
            "Closing this view does not approve new requests or stop sharing." : "Press Esc to stop sharing and release resources.");
        ui_footer(t->reason[0] ? t->reason : d->persistent ? "No allocation starts without your approval." : d->log, d->persistent ?
            t->phase == LMB_HOME_PENDING ? "↑ ↓ choose   Enter confirm   Esc detach   s stop sharing" :
            d->repeat_supported ? "Tab next model   x unload   r revoke repeat permission   Esc detach   s stop" :
            "Tab next model   x unload   Esc detach   s stop" : t->phase == LMB_HOME_PENDING ?
            "↑ ↓ choose   Enter confirm   Esc stop and return" :
            d->retained ? "x unload model, keep sharing   Esc stop sharing" : "Esc stop sharing and return");
        if (ui_w < 60 || ui_h < 28) {
            ui_begin("share resources"); ui_text(5, 4, UI_SAND, d->persistent ?
                "Resize to at least 60 × 28. Esc closes this view only." : "Resize to at least 60 × 28. Esc stops sharing.");
        }
        ui_present(); return;
    }
    printf("LUMABRI / SHARE RESOURCES\n\n%s · up to %.1f GB RAM · CPU\n\n%s\n",
           name, ram / 1e9, d->retained ? "Weights retained in RAM; ready for another chat" : lmb_home_phase_name(t->phase));
    if (t->phase != LMB_HOME_IDLE) {
        char who[65]; lmb_hex(who, t->offer.requester, 32);
        printf("\nRequester identity: %.24s…\nModel: %s (%s)\nLayers: %u–%u of %u\n"
               "RAM budget: %.2f GB · %s headroom: %.2f GB\n"
               "Context: %u · %u session slot(s) · %u threads · up to %u new tokens per turn\n",
               who, t->offer.model, t->offer.model_type, t->offer.begin,
               t->offer.end - 1, t->offer.layers, t->offer.ram_bytes / 1e9,
               d->resident ? "metadata" : "estimated disk",
               t->offer.disk_bytes / 1e9, t->offer.context, t->offer.sessions ? t->offer.sessions : 1,
               t->offer.threads < d->thread_capacity ? t->offer.threads : d->thread_capacity, t->offer.max_new);
        if (t->offer.runs_edge)
            puts("This computer also hosts chat and receives the conversation text.");
        else puts("This computer processes activations and keeps state for its layers.");
        if (t->reason[0]) printf("\n%s\n", t->reason);
    }
    puts(t->phase == LMB_HOME_PENDING ? "\n[y] Accept   [n] Decline   [q] Exit" :
                                      "\n[x] Stop and release   [q] Exit");
    printf("\nEngine log: %s\n", d->log);
    fflush(stdout);
}
#endif
