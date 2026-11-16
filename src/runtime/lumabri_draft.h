/* Drafts across the segments: setting, sources and observations.
 *
 * A draft never changes a token. Every proposed token travels the whole chain
 * as one provisional row and is kept only when the model, given the tokens
 * before it, selects exactly that token. What drafts change is how many
 * network rounds a reply costs. See docs/DRAFTS.md.
 */
#ifndef LUMABRI_DRAFT_H
#define LUMABRI_DRAFT_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/runtime/lumabri_draft_metrics.h"

/* Colibri's engine-neutral prompt lookup. Its gate formatter trips an
 * indentation warning that this code neither uses nor may change upstream. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "spec_draft.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define LMB_DRAFT_MAX_ROWS 16u    /* rows of one block, the certain row included */
#define LMB_DRAFT_DEFAULT_ROWS 5u /* one certain row and up to four drafts */
#define LMB_DRAFT_LOOKUP_MIN 2    /* shortest repeated run worth proposing from */
#define LMB_DRAFT_LOOKUP_MAX 4

typedef enum {
    LMB_DRAFT_OFF = 0,
    LMB_DRAFT_LOOKUP = 1, /* prompt lookup only: no weights, every family */
    LMB_DRAFT_HEAD = 2,   /* the model's own draft head only */
    LMB_DRAFT_AUTO = 3,   /* the head where the family has one, then lookup */
} LmbDraftMode;

typedef struct {
    LmbDraftMode mode;
    uint32_t rows; /* 2..LMB_DRAFT_MAX_ROWS */
} LmbDraftSetting;

/* The default is off: a block of K rows costs every segment K rows of
 * compute, and whether the saved rounds pay for the rejected rows depends on
 * the household's network and the reply. Turning it on is the operator's
 * (later the planner's) decision. 0 valid, -1 not a setting. */
static inline int lmb_draft_mode_parse(const char *text, LmbDraftMode *mode) {
    if (!text || !mode) return -1;
    if (!strcmp(text, "off") || !strcmp(text, "0")) *mode = LMB_DRAFT_OFF;
    else if (!strcmp(text, "lookup")) *mode = LMB_DRAFT_LOOKUP;
    else if (!strcmp(text, "head")) *mode = LMB_DRAFT_HEAD;
    else if (!strcmp(text, "auto") || !strcmp(text, "on") || !strcmp(text, "1"))
        *mode = LMB_DRAFT_AUTO;
    else return -1;
    return 0;
}

static inline const char *lmb_draft_mode_name(LmbDraftMode mode) {
    switch (mode) {
    case LMB_DRAFT_LOOKUP: return "lookup";
    case LMB_DRAFT_HEAD: return "head";
    case LMB_DRAFT_AUTO: return "auto";
    default: return "off";
    }
}

static inline int lmb_draft_rows_parse(const char *text, uint32_t *rows) {
    if (!text || !*text || !rows) return -1;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || end == text || *end || parsed < 2 || parsed > LMB_DRAFT_MAX_ROWS)
        return -1;
    *rows = (uint32_t)parsed;
    return 0;
}

/* Explicit arguments win over LUMABRI_DRAFTS / LUMABRI_DRAFT_BLOCK. A value
 * that is not a setting is an error, never silently "off" or "on". */
static inline int lmb_draft_setting(const char *mode_argument,
                                    const char *rows_argument,
                                    LmbDraftSetting *setting) {
    if (!setting) return -1;
    setting->mode = LMB_DRAFT_OFF;
    setting->rows = LMB_DRAFT_DEFAULT_ROWS;
    const char *mode = mode_argument ? mode_argument : getenv("LUMABRI_DRAFTS");
    const char *rows = rows_argument ? rows_argument : getenv("LUMABRI_DRAFT_BLOCK");
    if (mode && *mode && lmb_draft_mode_parse(mode, &setting->mode)) return -1;
    if (rows && *rows && lmb_draft_rows_parse(rows, &setting->rows)) return -1;
    return 0;
}

/* Prompt lookup: re-propose what followed the most recent earlier occurrence
 * of the tokens the history ends with. Needs no weights and no memory beyond
 * the token history the Edge already holds. */
static inline size_t lmb_draft_lookup(const int32_t *history, size_t count,
                                      int32_t *out, size_t want) {
    _Static_assert(sizeof(int) == sizeof(int32_t), "token ids are int");
    if (!history || !out || !want || count > (size_t)INT32_MAX) return 0;
    if (want > LMB_DRAFT_MAX_ROWS) want = LMB_DRAFT_MAX_ROWS;
    int found = spec_lookup((const int *)history, (int)count,
                            LMB_DRAFT_LOOKUP_MIN, LMB_DRAFT_LOOKUP_MAX,
                            (int)want, (int *)out);
    return found > 0 ? (size_t)found : 0;
}

/* Test instrument, in the spirit of Colibri's Q38_MTP_FORCE: LUMABRI_DRAFT_SCRIPT
 * is a comma-separated list where entry i is proposed for the reply's token i.
 * It lets a gate drive verification, commit and rollback with a chosen mix of
 * right and wrong drafts on a fixture whose own output never repeats. A draft
 * is still only ever kept when the model selects it, so a script cannot
 * change a reply. */
typedef struct {
    int32_t *tokens;
    size_t count;
} LmbDraftScript;

static inline int lmb_draft_script_load(LmbDraftScript *script) {
    if (!script) return -1;
    script->tokens = NULL;
    script->count = 0;
    const char *text = getenv("LUMABRI_DRAFT_SCRIPT");
    if (!text || !*text) return 0;
    size_t capacity = 1;
    for (const char *p = text; *p; p++) capacity += *p == ',';
    int32_t *tokens = (int32_t *)malloc(capacity * sizeof *tokens);
    if (!tokens) return -1;
    size_t count = 0;
    for (const char *p = text; ; ) {
        char *end = NULL;
        errno = 0;
        long value = strtol(p, &end, 10);
        if (errno || end == p || value < 0 || value > INT32_MAX ||
            (*end && *end != ',')) { free(tokens); return -1; }
        tokens[count++] = (int32_t)value;
        if (!*end) break;
        p = end + 1;
    }
    script->tokens = tokens;
    script->count = count;
    return 0;
}

/* Test instrument: LUMABRI_DRAFT_TRACE names a file that receives one line
 * per block the draft head proposed for, "position draft draft ...", so a
 * gate can compare the household head with the engine's own. */
static inline void lmb_draft_trace(uint64_t position, const int32_t *drafts, uint32_t count) {
    const char *path = getenv("LUMABRI_DRAFT_TRACE");
    if (!path || !*path || !count) return;
    FILE *out = fopen(path, "a");
    if (!out) return;
    fprintf(out, "%llu", (unsigned long long)position);
    for (uint32_t i = 0; i < count; i++) fprintf(out, " %d", drafts[i]);
    fputc('\n', out);
    fclose(out);
}

/* Proposals for the tokens after reply token `generated - 1`. */
static inline size_t lmb_draft_scripted(const LmbDraftScript *script,
                                        size_t generated,
                                        int32_t *out, size_t want) {
    size_t written = 0;
    while (script && written < want && generated + written < script->count) {
        out[written] = script->tokens[generated + written];
        written++;
    }
    return written;
}

#endif
