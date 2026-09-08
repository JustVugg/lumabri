/* Versioned observations from one completed generation. No benchmark or
 * calibration is implied by collecting timing from a real chat turn. */
#ifndef LUMABRI_METRICS_H
#define LUMABRI_METRICS_H
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t generated_tokens, decode_steps;
    double prefill_seconds, decode_seconds, total_seconds;
} LmbGenerationMetrics;

static inline int lmb_metrics_valid(const LmbGenerationMetrics *m) {
    return m && m->generated_tokens>0 && m->generated_tokens<=1048576 &&
        m->decode_steps==m->generated_tokens-1 &&
        isfinite(m->prefill_seconds) && m->prefill_seconds>=0 &&
        isfinite(m->decode_seconds) && m->decode_seconds>=0 &&
        isfinite(m->total_seconds) && m->total_seconds>=0 && m->total_seconds<=1e9 &&
        (m->decode_steps || m->decode_seconds==0) &&
        m->total_seconds+1e-6>=m->prefill_seconds+m->decode_seconds;
}

static inline double lmb_metrics_decode_rate(const LmbGenerationMetrics *m) {
    /* The first token comes from prefill, not a decode traversal. A one-token
     * reply cannot measure decode speed, even if selecting it took 1 us. */
    double rate=lmb_metrics_valid(m) && m->decode_steps && m->decode_seconds>0
        ? m->decode_steps/m->decode_seconds : 0;
    return isfinite(rate) ? rate : 0;
}

static inline int lmb_metrics_format(const LmbGenerationMetrics *m,char *out,size_t cap) {
    if(!lmb_metrics_valid(m)) return -1;
    int n=snprintf(out,cap,"PERF1 %u %u %.9f %.9f %.9f",m->generated_tokens,
        m->decode_steps,m->prefill_seconds,m->decode_seconds,m->total_seconds);
    return n<0 || (size_t)n>=cap ? -1 : 0;
}

/* 0 valid; 1 absent on a legacy engine; -1 malformed. Never interpret a
 * truncated/new-version observation as an exact historical calibration. */
static inline int lmb_metrics_parse(const char *stat,LmbGenerationMetrics *out) {
    memset(out,0,sizeof *out);
    const char *p=strstr(stat," PERF1 ");
    if(!p) return strstr(stat," PERF") ? -1 : 1;
    p+=7;
    uint32_t *counts[]={&out->generated_tokens,&out->decode_steps};
    for(unsigned i=0;i<2;i++) {
        if(*p<'0' || *p>'9') return -1;
        errno=0; char *end; unsigned long n=strtoul(p,&end,10);
        if(errno || n>1048576 || *end!=' ') return -1;
        *counts[i]=(uint32_t)n; p=end+1;
    }
    double *times[]={&out->prefill_seconds,&out->decode_seconds,&out->total_seconds};
    for(unsigned i=0;i<3;i++) {
        if(*p<'0' || *p>'9') return -1;
        errno=0; char *end; double value=strtod(p,&end);
        if(errno || !isfinite(value) || value<0) return -1;
        *times[i]=value;
        if(i<2) {if(*end!=' ')return -1;p=end+1;}
        else p=end;
    }
    while(*p==' ' || *p=='\r' || *p=='\n') p++;
    if(*p || !lmb_metrics_valid(out)) {memset(out,0,sizeof *out);return -1;}
    return 0;
}
#endif
