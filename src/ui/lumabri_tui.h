/* lumabri_tui.h — the screen a person actually looks at.
 *
 * Full screen, C only, no dependency. What it renders is the planner's
 * snapshot and nothing else: the screen may never know a fact the planner
 * does not, which is what keeps a display from becoming a second source of
 * truth that drifts from the first.
 *
 * Two views, because two are what the product needs and a third would be
 * invented rather than required. The catalogue is a list of models with
 * their state; the detail is one model with what it needs, what is missing,
 * and where its layers would go. Everything else — panels, tabs, widgets —
 * exists only in as much as those two views use it. */
#ifndef LUMABRI_TUI_H
#define LUMABRI_TUI_H

#include "lumabri_cluster.h"
#include "lumabri_calibration.h"
#include "lumabri_machine.h"
#include "src/planner/lumabri_catalogue_advice.h"

#define LMB_TUI_MAX_MODELS 64

typedef struct {
    char name[64];
    char dir[512];
    LmbModelShape shape;
    LmbClusterPlan plan;
    int planned;                    /* 0 when the cluster cannot be planned */
    int weights_present;
    uint64_t checkpoint_bytes;
    int checkpoint_inventory_ok;
    LmbCalibration calibration;    /* owned by the snapshot, never a borrowed pointer */
    int has_calibration;
    char content_id[65];
    LmbCalKey calibration_key;      /* exact current conditions */
    int calibration_key_valid;
    uint32_t advice_flags;
} LmbTuiModel;

typedef struct LmbTuiState {
    LmbTuiModel models[LMB_TUI_MAX_MODELS];
    int nmodels;
    LmbClusterNode nodes[LMB_CLUSTER_MAX_NODES];
    LmbMachineProfile profiles[LMB_CLUSTER_MAX_NODES];
    char identities[LMB_CLUSTER_MAX_NODES][65];
    char runtime_ids[LMB_CLUSTER_MAX_NODES][65];
    uint32_t ages_ms[LMB_CLUSTER_MAX_NODES];
    uint32_t nnodes;
    int inventory_ok;
    char build_id[65];
    int action_model;
    int quick_calibration;
    int initial_tab;
    char selected_nodes[LMB_CLUSTER_MAX_NODES][65];
    uint32_t context, sessions, max_new;
    char root[512];                 /* where the checkpoints were found */
    char disk[512];
    char tracker[256];
    int (*refresh)(struct LmbTuiState *state, void *context);
    void *refresh_context;
} LmbTuiState;

enum { LMB_TUI_REQUEST_CHAT = 10, LMB_TUI_REQUEST_CALIBRATION = 11 };

/* Selection changes precede asynchronous planning. Never render a speed
 * attached to the previous selection during that refresh window. */
static inline void lmb_tui_invalidate_plans(LmbTuiState *st) {
    for (int i = 0; i < st->nmodels; i++) {
        st->models[i].planned = 0;
        st->models[i].calibration_key_valid = 0;
        st->models[i].advice_flags = 0;
    }
}

static inline int lmb_tui_node_enabled(const LmbTuiState *st, uint32_t node) {
    if (node >= st->nnodes || !st->nodes[node].addr[0]) return 0;
    for (uint32_t i = 0; i < LMB_CLUSTER_MAX_NODES; i++)
        if (st->selected_nodes[i][0] && !strcmp(st->selected_nodes[i], st->identities[node])) return 1;
    return 0;
}

/* Run the interface. `snapshot` renders one frame to stdout and returns
 * instead of taking the terminal, so the screen can be tested without a pty;
 * `keys` is a synthetic key sequence for the same reason, NULL for a real
 * session. Returns 0 on a clean exit. */
int lmb_tui_run(LmbTuiState *st, int snapshot, const char *keys);

#endif /* LUMABRI_TUI_H */
