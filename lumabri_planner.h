/* lumabri_planner.h — what a checkpoint costs, per adapter, before anything
 * is loaded.
 *
 * The catalogue has to say "this cluster can run that model, split this way,
 * ready in about this long" without opening an engine, so somebody has to
 * turn a checkpoint into memory figures. The Colibri runtime ABI exposes no
 * C sizing call (its Python control plane is not linked here), so the
 * description lives in Lumabri's adapter layer and Colibri stays untouched.
 *
 * Three numbers matter and they are not the same number:
 *
 *   resident_bytes      every weight of the range in RAM. The fast mode.
 *   working_set_bytes   the least that must be resident for the range to run
 *                       at all: dense weights, a top-k expert cache, kernel
 *                       scratch. Below this the node cannot start, whatever
 *                       the disk can stream.
 *   state_bytes         KV and recurrent state, which scale with context and
 *                       sessions and belong to neither of the above.
 *
 * The gap between the first two is the whole disk mode: a range whose
 * resident cost does not fit but whose working set does can run, reading the
 * rest from NVMe or CAS — and only where the adapter says it may.
 *
 * Every figure is derived from config.json and declared arithmetic. Nothing
 * here is measured, and nothing here may be printed as a speed: a planner
 * says what fits, a calibration says how fast. */
#ifndef LUMABRI_PLANNER_H
#define LUMABRI_PLANNER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Header-only helpers: a translation unit uses the few it needs. */
#if defined(__GNUC__)
#define LMB_UNUSED __attribute__((unused))
#else
#define LMB_UNUSED
#endif
#include "lumabri_families.h"

/* How a family's weights are laid out, in the terms the estimates need. All
 * of it comes out of config.json; a field the checkpoint does not carry is
 * zero, and an estimate that needs a zero field says it cannot answer. */
typedef struct {
    char model_type[64];
    char segment_id[32];
    uint32_t layers;
    uint32_t hidden;
    uint32_t intermediate;      /* dense FFN width */
    uint32_t moe_intermediate;  /* expert width; 0 when the model is dense */
    uint32_t experts;           /* routed experts per layer; 0 when dense */
    uint32_t experts_per_tok;   /* top-k */
    uint32_t heads, kv_heads;
    uint32_t vocab;
    uint32_t bits_per_weight;   /* 16 bf16, 8, 4 for fp4 checkpoints */
    int disk_streaming;         /* the adapter has DEMONSTRATED disk mode */
    int sizing_verified;        /* adapter-specific arithmetic was verified */
} LmbModelShape;

typedef struct {
    uint64_t resident_bytes;    /* every weight of the range */
    uint64_t working_set_bytes; /* the least that must be resident to run */
    uint64_t state_bytes;       /* KV and recurrent state for the request */
    uint64_t scratch_bytes;     /* kernel workspace, per session */
    int ok;                     /* 0 when the shape cannot answer */
} LmbRangeCost;

/* bits→bytes with the rounding the stores actually use: block scales and
 * alignment add roughly a fifth on the fp4 path, which is the figure
 * lmbe_expert_bytes has used since the expert store was written. */
static uint64_t LMB_UNUSED lmb_weight_bytes(uint64_t elements, uint32_t bits) {
    if (!bits) bits = 16;
    uint64_t raw = elements * bits / 8u;
    return bits < 8 ? raw + raw / 5u : raw;
}

static uint64_t LMB_UNUSED lmb_expert_bytes_of(const LmbModelShape *m) {
    if (!m->experts || !m->moe_intermediate) return 0;
    /* gate, up, down: three hidden × moe_intermediate matrices */
    return lmb_weight_bytes((uint64_t)m->hidden * m->moe_intermediate * 3u,
                            m->bits_per_weight);
}

/* Attention plus the dense feed-forward of one layer — everything that is
 * not a routed expert, and therefore everything a Segment node must hold
 * whatever its cache policy is. */
static uint64_t LMB_UNUSED lmb_dense_layer_bytes(const LmbModelShape *m) {
    uint64_t head_dim = m->heads ? (uint64_t)m->hidden / m->heads : 0;
    uint64_t kv = m->kv_heads ? (uint64_t)m->kv_heads * head_dim : m->hidden;
    uint64_t attn = (uint64_t)m->hidden * m->hidden      /* q */
                  + (uint64_t)m->hidden * kv * 2u        /* k, v */
                  + (uint64_t)m->hidden * m->hidden;     /* o */
    uint64_t ffn = m->experts ? 0u
                 : (uint64_t)m->hidden * m->intermediate * 3u;
    return lmb_weight_bytes(attn + ffn, m->bits_per_weight);
}

/* Embedding and head, which live wherever Edge runs and nowhere else. */
static uint64_t LMB_UNUSED lmb_edge_bytes(const LmbModelShape *m) {
    if (!m->vocab || !m->hidden) return 0;
    return lmb_weight_bytes((uint64_t)m->vocab * m->hidden * 2u,
                            m->bits_per_weight);
}

/* KV for one session over `context` tokens across `layers` layers, at f32 —
 * the state travels and is compared between nodes, so it is not quantised. */
static uint64_t LMB_UNUSED lmb_kv_bytes(const LmbModelShape *m, uint32_t layers,
                             uint32_t context, uint32_t sessions) {
    uint64_t head_dim = m->heads ? (uint64_t)m->hidden / m->heads : 0;
    uint64_t per_tok = m->kv_heads ? (uint64_t)m->kv_heads * head_dim * 2u
                                   : (uint64_t)m->hidden * 2u;
    return per_tok * context * layers * (sessions ? sessions : 1) * 4u;
}

/* What one node pays to hold layers [begin, end). */
static LmbRangeCost LMB_UNUSED lmb_estimate_segment(const LmbModelShape *m,
                                         uint32_t begin, uint32_t end,
                                         uint32_t context, uint32_t sessions) {
    LmbRangeCost c;
    memset(&c, 0, sizeof c);
    if (!m->sizing_verified || !m->layers || begin >= end ||
        end > m->layers || !m->hidden) return c;
    uint32_t n = end - begin;

    uint64_t dense = lmb_dense_layer_bytes(m) * n;
    uint64_t expert = lmb_expert_bytes_of(m);
    uint64_t all_experts = expert * m->experts * n;

    /* The floor: the dense part, plus enough expert slots for the top-k of
     * every layer in the range. A cache below that thrashes on a single
     * token — it is not a slower mode, it is a broken one. */
    uint32_t topk = m->experts_per_tok ? m->experts_per_tok : 1;
    if (topk > m->experts && m->experts) topk = m->experts;
    uint64_t floor_experts = expert * topk * n;

    c.state_bytes = lmb_kv_bytes(m, n, context ? context : 4096, sessions);
    /* scratch is dominated by the widest matmul the layer runs */
    uint64_t widest = m->experts ? m->moe_intermediate : m->intermediate;
    c.scratch_bytes = (uint64_t)widest * 4u * 8u;   /* f32, a few rows */
    c.resident_bytes = dense + all_experts;
    c.working_set_bytes = dense + floor_experts;
    c.ok = 1;
    return c;
}

/* What the machine running Edge pays: embedding, head, and the state it
 * keeps for the sessions it owns. */
static LmbRangeCost LMB_UNUSED lmb_estimate_edge(const LmbModelShape *m,
                                      uint32_t context, uint32_t sessions) {
    LmbRangeCost c;
    memset(&c, 0, sizeof c);
    if (!m->sizing_verified || !m->vocab || !m->hidden) return c;
    c.resident_bytes = c.working_set_bytes = lmb_edge_bytes(m);
    c.state_bytes = (uint64_t)(context ? context : 4096) * m->hidden * 4u *
                    (sessions ? sessions : 1);
    c.ok = 1;
    return c;
}

/* The three states of the catalogue. Disk is not "it did not fit": it is a
 * mode the adapter has demonstrated, with the working set resident and the
 * rest reachable. An adapter that has not demonstrated it does not get it. */
typedef enum {
    LMB_PLAN_RESIDENT = 0,
    LMB_PLAN_DISK,
    LMB_PLAN_UNRUNNABLE,
} LmbPlanState;

static LmbPlanState LMB_UNUSED lmb_plan_state(const LmbModelShape *m,
                                   const LmbRangeCost *cost,
                                   uint64_t budget_bytes) {
    if (!cost->ok) return LMB_PLAN_UNRUNNABLE;
    uint64_t live = cost->state_bytes + cost->scratch_bytes;
    if (cost->resident_bytes + live <= budget_bytes) return LMB_PLAN_RESIDENT;
    if (m->disk_streaming && cost->working_set_bytes + live <= budget_bytes)
        return LMB_PLAN_DISK;
    return LMB_PLAN_UNRUNNABLE;
}

static LMB_UNUSED const char *lmb_plan_state_name(LmbPlanState s) {
    switch (s) {
    case LMB_PLAN_RESIDENT:  return "resident";
    case LMB_PLAN_DISK:      return "from disk";
    default:                 return "not runnable";
    }
}

/* Read the shape out of a checkpoint's config.json.
 *
 * Deliberately tolerant: a field the checkpoint does not carry stays zero,
 * and an estimate that needs a zero field refuses instead of guessing. The
 * alternative — defaults — is how a catalogue ends up promising a model it
 * cannot size. Returns 0 when at least the layer count and hidden size were
 * found, which is the minimum any estimate needs. */
/* Small, scope-aware JSON member lookup. It intentionally handles only the
 * primitive/object forms config.json needs, but unlike strstr it never picks
 * a same-named field from a nested vision config. */
static const char *LMB_UNUSED lmb_json_member(const char *object,
                                              const char *key) {
    if (!object || *object != '{') return NULL;
    int depth = 1, in_string = 0, escape = 0;
    for (const char *p = object + 1; *p && depth; p++) {
        if (in_string) {
            if (escape) { escape = 0; continue; }
            if (*p == '\\') { escape = 1; continue; }
            if (*p == '"') in_string = 0;
            continue;
        }
        if (*p == '{' || *p == '[') { depth++; continue; }
        if (*p == '}' || *p == ']') { depth--; continue; }
        if (*p != '"') continue;
        const char *start = p + 1, *q = start;
        int esc = 0;
        for (; *q; q++) {
            if (esc) { esc = 0; continue; }
            if (*q == '\\') { esc = 1; continue; }
            if (*q == '"') break;
        }
        if (!*q) return NULL;
        if (depth == 1 && (size_t)(q - start) == strlen(key) &&
            !memcmp(start, key, (size_t)(q - start))) {
            const char *v = q + 1;
            while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
            if (*v != ':') return NULL;
            do { v++; } while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n');
            return v;
        }
        p = q;
    }
    return NULL;
}

static int LMB_UNUSED lmb_json_u32(const char *object, const char *key,
                                   uint32_t *out) {
    const char *v = lmb_json_member(object, key);
    if (!v || *v < '0' || *v > '9') return -1;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || n == 0 || n > UINT32_MAX) return -1;
    *out = (uint32_t)n;
    return 0;
}

static int LMB_UNUSED lmb_json_string(const char *object, const char *key,
                                      char *out, size_t cap) {
    const char *v = lmb_json_member(object, key);
    if (!v || *v != '"' || cap == 0) return -1;
    const char *end = v + 1;
    while (*end && *end != '"') {
        if (*end == '\\') return -1; /* model_type never needs escapes */
        end++;
    }
    size_t n = (size_t)(end - (v + 1));
    if (*end != '"' || n >= cap) return -1;
    memcpy(out, v + 1, n); out[n] = 0;
    return 0;
}

static LMB_UNUSED int lmb_shape_from_config(const char *model_dir,
                                            LmbModelShape *out) {
    memset(out, 0, sizeof *out);
    if (!model_dir || !model_dir[0]) return -1;
    char path[1024];
    snprintf(path, sizeof path, "%s/config.json", model_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    if (lmb_json_string(buf, "model_type", out->model_type,
                        sizeof out->model_type)) return -1;
    const LmbModelFamily *fam = lmb_family_for(out->model_type);
    if (!fam) return -1;
    snprintf(out->segment_id, sizeof out->segment_id, "%s", fam->segment_id);

    const char *cfg = buf;
    if (!strcmp(fam->config_section, "text_config")) {
        const char *nested = lmb_json_member(buf, "text_config");
        /* Text-only exports put their text fields at the root. Vision wrappers
         * must provide a real text_config object. */
        if (nested && *nested == '{') cfg = nested;
    }
    struct { const char *key; uint32_t *slot; } nums[] = {
        { "num_hidden_layers",    &out->layers },
        { "hidden_size",          &out->hidden },
        { "intermediate_size",    &out->intermediate },
        { "moe_intermediate_size",&out->moe_intermediate },
        { "n_routed_experts",     &out->experts },
        { "num_experts",          &out->experts },
        { "num_experts_per_tok",  &out->experts_per_tok },
        { "num_attention_heads",  &out->heads },
        { "num_key_value_heads",  &out->kv_heads },
        { "vocab_size",           &out->vocab },
    };
    for (size_t i = 0; i < sizeof nums / sizeof *nums; i++) {
        uint32_t v = 0;
        if (!lmb_json_u32(cfg, nums[i].key, &v) && !*nums[i].slot)
            *nums[i].slot = v;
    }
    /* Quantisation is not in config.json for every family, so it is declared
     * per family rather than guessed: fp4 for V4, bf16 elsewhere until a
     * checkpoint of that family says otherwise. */
    out->bits_per_weight = !strcmp(fam->segment_id, "deepseek_v4") ? 4 : 16;
    if (!out->kv_heads) out->kv_heads = out->heads;
    /* Not every family names the expert width separately. OLMoE routes every
     * layer and its experts are `intermediate_size` wide, so a checkpoint
     * with experts but no moe_intermediate_size is a MoE whose experts are
     * the dense width — reading it as dense would put its whole expert set
     * in the working set and make disk mode indistinguishable from resident. */
    if (out->experts && !out->moe_intermediate)
        out->moe_intermediate = out->intermediate;
    /* The current formula is verified only for the two fixtures against which
     * it was written. Other adapters remain visible through the family table,
     * but cannot produce a fit decision until their adapter-specific sizing
     * callback lands. This is preferable to a confident under-allocation. */
    out->sizing_verified = !strcmp(fam->segment_id, "olmoe") ||
                           !strcmp(fam->segment_id, "deepseek_v4");
    return out->layers && out->hidden ? 0 : -1;
}

#endif /* LUMABRI_PLANNER_H */
