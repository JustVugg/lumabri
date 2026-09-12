/* Immutable view of an approved household plan. It is not a performance
 * measurement, a live counter, or proof that two donors are physical PCs. */
#ifndef LUMABRI_EXECUTION_VIEW_H
#define LUMABRI_EXECUTION_VIEW_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lumabri_cluster.h"

typedef struct {
    char name[64], address[64];
    uint32_t begin, end;
    uint64_t reserved_bytes;
    int edge;
} LmbExecutionNode;

typedef struct {
    uint32_t count, layers;
    uint32_t hybrid;
    LmbExecutionNode nodes[LMB_CLUSTER_MAX_NODES];
} LmbExecutionView;

static int lmb_execution_valid(const LmbExecutionView *view) {
    if (!view || !view->count || view->count > LMB_CLUSTER_MAX_NODES || !view->layers)
        return 0;
    uint32_t next = 0, edges = 0;
    for (uint32_t i = 0; i < view->count; i++) {
        const LmbExecutionNode *n = &view->nodes[i];
        if (!n->name[0] || !n->address[0] || !n->reserved_bytes ||
            !memchr(n->name, 0, sizeof n->name) ||
            !memchr(n->address, 0, sizeof n->address) ||
            (!view->hybrid && n->begin != next) || n->end <= n->begin || n->end > view->layers ||
            (n->edge != 0 && n->edge != 1)) return 0;
        if (view->hybrid && (view->hybrid != 1 || view->count < 2 ||
            (!i && (!n->edge || n->begin || n->end != view->layers)) ||
            (i && n->edge) || (i > 1 && n->begin < next))) return 0;
        next = n->end; edges += (uint32_t)n->edge;
    }
    return (view->hybrid || next == view->layers) && edges == 1;
}

/* Names come from network inventory. Never let terminal control bytes in a
 * peer name turn this read-only view into terminal commands. */
static void lmb_execution_label(FILE *out, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        fputc(*p >= 32 && *p < 127 ? *p : '?', out);
}

static void lmb_execution_print(FILE *out, const LmbExecutionView *view) {
    if (!lmb_execution_valid(view)) {
        fputs("  No approved household plan is attached to this chat.\n", out);
        return;
    }
    fprintf(out, "\n  Approved %s plan: %u compute donor%s, %u layers\n",
            view->hybrid ? "Hybrid" : "Segment", view->count, view->count == 1 ? "" : "s", view->layers);
    fputs("  All listed segments acknowledged READY before this chat opened.\n", out);
    for (uint32_t i = 0; i < view->count; i++) {
        const LmbExecutionNode *n = &view->nodes[i];
        fputs("  ", out); lmb_execution_label(out, n->name);
        fputs(" (", out); lmb_execution_label(out, n->address);
        fprintf(out, ") · layers [%u,%u) · %.2f GB reserved%s\n",
                n->begin, n->end, n->reserved_bytes / 1e9,
                n->edge ? (view->hybrid ? " · full resident coordinator / chat host" : " · Edge / chat host") :
                          (view->hybrid ? " · approved expert accelerator" : ""));
    }
    fputs(view->hybrid ? "  Coordinator: full local fallback. Other donors: concurrent resident experts.\n" :
                        "  Each Segment executes attention and experts for its own layers.\n", out);
    fputs(
          "  This chat process runs no model layers.\n"
          "  A separate approved donor may run on the same computer.\n"
          "  /plan shows this allocation; /experts shows tracker activity.\n", out);
}
#endif
