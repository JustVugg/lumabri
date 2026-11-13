/* Shared checked metadata integers and qwen36 family memory contracts. */
#ifndef LUMABRI_PLAN_QWEN36_H
#define LUMABRI_PLAN_QWEN36_H
#include <errno.h>
#include "tensors.h"

static int LMB_UNUSED lmb_plan_u32(const char *json, const char *key,
                                  uint32_t minimum, uint32_t maximum, uint32_t *out) {
    const char *p = lmb_json_member(json, key);
    if (!p || *p < '0' || *p > '9') return -1;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return -1;
    errno = 0; char *end;
    unsigned long long value = strtoull(p, &end, 10);
    if (errno || value < minimum || value > maximum) return -1;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end != ',' && *end != '}' && *end != ']') return -1;
    *out = (uint32_t)value;
    return 0;
}

#include "qwen36_variants.h"
#include "qwen36_gguf.h"
static int LMB_UNUSED lmb_qwen36_memory(const char *root, const char *cfg, LmbModelShape *m) {
    LmbGguf g={0};int native=lmb_gguf_model(root,&g);
    if(native!=1) {
        int rc=native?-1:lmb_qwen_gguf_memory(&g,m);
        lmb_gguf_free(&g);return rc;
    }
    return lmb_q36_variants_memory(root,cfg,m);
}
#endif
