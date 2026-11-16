/* Observations of a reply decoded with drafts (docs/DRAFTS.md). No engine
 * dependency: the chatter writes them and the planner reads them. */
#ifndef LUMABRI_DRAFT_METRICS_H
#define LUMABRI_DRAFT_METRICS_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Observations of one completed reply. A round is one traversal of the whole
 * chain during decode, whatever it carried. `tokens` are the tokens those
 * rounds selected, so tokens/rounds is the accepted tokens per network
 * round and rounds/tokens the cost the milestone asks to bring below one. */
typedef struct {
    uint32_t rounds;
    uint32_t tokens;
    uint32_t proposed; /* draft rows sent */
    uint32_t accepted; /* draft rows whose output selected a token */
    uint32_t discards; /* extra rounds that only took provisional rows back */
} LmbDraftMetrics;

static inline int lmb_draft_metrics_valid(const LmbDraftMetrics *m) {
    return m && m->rounds && m->rounds <= 1048576 && m->tokens <= 1048576 &&
           m->discards <= m->rounds && m->tokens >= m->rounds - m->discards &&
           m->accepted <= m->proposed && m->proposed <= 16777216 &&
           m->accepted == m->tokens - (m->rounds - m->discards);
}

/* STAT extension, after STOP1 and before LINKS1/STAGES1/PERF1 so readers of
 * those keep working: "DRAFTS1 rounds tokens proposed accepted discards". */
static inline int lmb_draft_metrics_format(const LmbDraftMetrics *m,
                                           char *out, size_t cap) {
    if (!lmb_draft_metrics_valid(m)) return -1;
    int n = snprintf(out, cap, "DRAFTS1 %u %u %u %u %u", m->rounds, m->tokens,
                     m->proposed, m->accepted, m->discards);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

/* 0 valid; 1 absent (no drafts, or an older engine); -1 malformed. */
static inline int lmb_draft_metrics_parse(const char *stat, LmbDraftMetrics *out) {
    if (!stat || !out) return -1;
    memset(out, 0, sizeof *out);
    const char *p = strstr(stat, " DRAFTS1 ");
    if (!p) return strstr(stat, " DRAFTS") ? -1 : 1;
    p += 9;
    LmbDraftMetrics parsed = {0};
    uint32_t *fields[] = { &parsed.rounds, &parsed.tokens, &parsed.proposed,
                           &parsed.accepted, &parsed.discards };
    for (unsigned i = 0; i < 5; i++) {
        if (*p < '0' || *p > '9') return -1;
        errno = 0;
        char *end;
        unsigned long value = strtoul(p, &end, 10);
        if (errno || value > 16777216 || (*end != ' ' && *end && *end != '\n' && *end != '\r'))
            return -1;
        *fields[i] = (uint32_t)value;
        p = *end == ' ' ? end + 1 : end;
    }
    if (!lmb_draft_metrics_valid(&parsed)) return -1;
    *out = parsed;
    return 0;
}

/* Network rounds per token; 0 when nothing was decoded. */
static inline double lmb_draft_rounds_per_token(const LmbDraftMetrics *m) {
    return lmb_draft_metrics_valid(m) && m->tokens
        ? (double)m->rounds / (double)m->tokens : 0;
}

#endif
