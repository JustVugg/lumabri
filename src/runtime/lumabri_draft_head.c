#include "src/runtime/lumabri_draft_head.h"

#include <string.h>

static LmbDraftHeadBinding g_head;
static int g_head_set;

int lmb_draft_head_register(const LmbDraftHeadBinding *binding) {
    if (!binding || !binding->owner || !binding->session_create || !binding->session_destroy ||
        !binding->commit || !binding->propose || g_head_set) return -1;
    g_head = *binding;
    g_head_set = 1;
    return 0;
}

void lmb_draft_head_unregister(void *owner) {
    if (!g_head_set || g_head.owner != owner) return;
    memset(&g_head, 0, sizeof g_head);
    g_head_set = 0;
}

const LmbDraftHeadBinding *lmb_draft_head(void) {
    return g_head_set ? &g_head : NULL;
}
