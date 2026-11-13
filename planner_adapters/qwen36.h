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
static int LMB_UNUSED lmb_qwen36_memory(const char *root, const char *cfg, LmbModelShape *m) {
    return lmb_q36_variants_memory(root,cfg,m);
}
#endif
