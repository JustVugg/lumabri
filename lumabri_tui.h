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

#define LMB_TUI_MAX_MODELS 64

typedef struct {
    char name[64];
    char dir[512];
    LmbModelShape shape;
    LmbClusterPlan plan;
    int planned;                    /* 0 when the cluster cannot be planned */
    const LmbCalibration *calibration;   /* NULL until something is measured */
} LmbTuiModel;

typedef struct {
    LmbTuiModel models[LMB_TUI_MAX_MODELS];
    int nmodels;
    LmbClusterNode nodes[LMB_CLUSTER_MAX_NODES];
    uint32_t nnodes;
    uint32_t context, sessions;
    char root[512];                 /* where the checkpoints were found */
} LmbTuiState;

/* Run the interface. `snapshot` renders one frame to stdout and returns
 * instead of taking the terminal, so the screen can be tested without a pty;
 * `keys` is a synthetic key sequence for the same reason, NULL for a real
 * session. Returns 0 on a clean exit. */
int lmb_tui_run(LmbTuiState *st, int snapshot, const char *keys);

#endif /* LUMABRI_TUI_H */
