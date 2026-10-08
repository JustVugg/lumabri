/* Gateway-observed latency, not kernel time or client display time. DATA
 * fragments are NOT tokens. Only consecutive DECODE notifications can form
 * token-gap statistics. Missing/malformed telemetry never breaks a reply.
 * Keep every gap up to the fixed bound; beyond it withhold quantiles rather
 * than silently reporting a truncated or sampled distribution. */
#ifndef LMB_RESPONSE_METRICS_H
#define LMB_RESPONSE_METRICS_H
#include "lumabri_metrics.h"

#define LMB_RESPONSE_GAPS 4096u
typedef struct {
    double started, first_text, last_text, first_token, last_token;
    double gaps[LMB_RESPONSE_GAPS];
    uint32_t tokens, gap_count;
    int invalid, text_seen;
} LmbResponseMetrics;
typedef struct {
    double ttft_seconds, generation_seconds, completion_seconds;
    double token_first_seconds, gap_p50_seconds, gap_p95_seconds;
    uint32_t tokens, gaps;
    int text_known, token_times_known, quantiles_known;
} LmbResponseSummary;

static inline void lmb_response_text(LmbResponseMetrics *m, double now, size_t bytes) {
    if (!m || !bytes || !isfinite(now) || now<m->started || (m->text_seen && now<m->last_text)) return;
    if (!m->text_seen) { m->first_text=now; m->text_seen=1; }
    m->last_text=now;
}
static inline int lmb_response_uint(const char **cursor, uint32_t *out) {
    const char *p=*cursor; uint32_t n=0;
    if (*p<'0' || *p>'9') return -1;
    do {
        if (n>(1048576u-(unsigned)(*p-'0'))/10) return -1;
        n=n*10+(unsigned)(*p++-'0');
    } while (*p>='0' && *p<='9');
    *cursor=p; *out=n; return 0;
}
/* The shared reply decoder has already bound this line to the request ID. */
static inline void lmb_response_progress(LmbResponseMetrics *m, const char *line, double now) {
    if (!m || m->invalid || !line || strncmp(line,"PROGRESS ",9)) return;
    const char *p=strchr(line+9,' ');
    if (!p || strncmp(++p,"DECODE",6) || (p[6] && p[6]!=' ')) return;
    if (p[6]!=' ') { m->invalid=1; return; }
    p+=7; uint32_t count=0,limit=0;
    if (lmb_response_uint(&p,&count) || !count || m->tokens==1048576u || count!=m->tokens+1 ||
        !isfinite(now) || now<m->started || (m->tokens && now<m->last_token)) { m->invalid=1; return; }
    if (*p==' ') {
        p++;
        if (lmb_response_uint(&p,&limit) || limit<count) { m->invalid=1; return; }
    }
    if (*p) { m->invalid=1; return; }
    if (!m->tokens) m->first_token=now;
    else if (m->gap_count<LMB_RESPONSE_GAPS) m->gaps[m->gap_count++]=now-m->last_token;
    m->last_token=now; m->tokens=count;
}
static inline int lmb_response_double_order(const void *a, const void *b) {
    double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y);
}
/* Called only for a fully completed reply. Replay invalidates token times:
 * replay notifications include hidden prefix work, not new visible tokens. */
static inline int lmb_response_summary(LmbResponseMetrics *m, const char *stat, uint32_t attempts,
    double generation_end, double completed, LmbResponseSummary *out) {
    if (!out) return -1;
    memset(out,0,sizeof *out);
    if (!m || !attempts || !isfinite(m->started) || !isfinite(generation_end) || !isfinite(completed) ||
        m->started<0 || generation_end<m->started || completed<generation_end ||
        (m->text_seen && m->last_text>generation_end)) return -1;
    out->generation_seconds=generation_end-m->started; out->completion_seconds=completed-m->started;
    out->text_known=m->text_seen;
    if (out->text_known) out->ttft_seconds=m->first_text-m->started;
    LmbGenerationMetrics engine;
    if (attempts!=1 || m->invalid || lmb_metrics_parse(stat,&engine) ||
        !m->tokens || m->tokens!=engine.generated_tokens || m->last_token>generation_end) return 0;
    out->token_times_known=1; out->tokens=m->tokens;
    out->token_first_seconds=m->first_token-m->started; out->gaps=m->tokens-1;
    if (!out->gaps || out->gaps!=m->gap_count) return 0;
    qsort(m->gaps,m->gap_count,sizeof *m->gaps,lmb_response_double_order);
    out->quantiles_known=1;
    out->gap_p50_seconds=m->gaps[(m->gap_count+1)/2-1];
    out->gap_p95_seconds=m->gaps[(95*m->gap_count+99)/100-1];
    return 0;
}
#endif
