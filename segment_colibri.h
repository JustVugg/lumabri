/* Shared, additive bridge to Colibri's public Segment/Edge ABI. */
#ifndef LUMABRI_SEGMENT_COLIBRI_H
#define LUMABRI_SEGMENT_COLIBRI_H

#include "edge_adapters.h"
#include "edge_runtime.h"
#include "segment_adapters.h"
#include "segment_runtime.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int lmb_colibri_register_all(void) {
    return coli_glm_segment_adapter_register() ||
           coli_inkling_segment_adapter_register() ||
           coli_kimi_segment_adapter_register() ||
           coli_olmoe_segment_adapter_register() ||
           coli_qwen36_segment_adapter_register() ||
           coli_deepseek_v4_segment_adapter_register() ||
           coli_glm_edge_adapter_register() ||
           coli_inkling_edge_adapter_register() ||
           coli_kimi_edge_adapter_register() ||
           coli_olmoe_edge_adapter_register() ||
           coli_qwen36_edge_adapter_register() ||
           coli_deepseek_v4_edge_adapter_register();
}

/* Colibri's ABI is allowed to return a failure without writing the caller's
 * error buffer. A buffer that starts as uninitialised stack is then printed
 * as one: an operator chasing a real MoE failure was shown
 * "[segment-node] run: q<garbage>v" instead of a reason. Every error buffer
 * in the Segment binaries starts empty and is read back through here. */
static const char *engine_error(const char *error) {
    return error && error[0] ? error : "the engine reported no reason";
}

static int lmb_hex_root(const char *hex, uint8_t out[32]) {
    if (!hex || strlen(hex) != 64) return -1;
    unsigned any = 0;
    for (size_t i = 0; i < 32; i++) {
        unsigned hi, lo;
        char a = hex[2 * i], b = hex[2 * i + 1];
        if (a >= '0' && a <= '9') hi = (unsigned)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (unsigned)(a - 'a' + 10);
        else if (a >= 'A' && a <= 'F') hi = (unsigned)(a - 'A' + 10);
        else return -1;
        if (b >= '0' && b <= '9') lo = (unsigned)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (unsigned)(b - 'a' + 10);
        else if (b >= 'A' && b <= 'F') lo = (unsigned)(b - 'A' + 10);
        else return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
        any |= out[i];
    }
    return any ? 0 : -1;
}

static size_t lmb_state_bytes(uint32_t rows, uint32_t width, uint32_t dtype) {
    size_t element = dtype == 1 ? 4u : (dtype == 2 || dtype == 3 ? 2u : 0u);
    if (!element || !rows || !width || width > SIZE_MAX / rows ||
        (size_t)width * rows > SIZE_MAX / element) return 0;
    return (size_t)rows * width * element;
}

static int lmb_parse_u32(const char *text, uint32_t minimum,
                         uint32_t maximum, uint32_t *value) {
    if (!text || !*text || !value || minimum > maximum) return -1;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || parsed < minimum || parsed > maximum)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

#endif
