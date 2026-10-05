/* One completed turn -> one provenance-bound observation, shared by TUI and
 * API. No bytes/token guesses and no measurement from legacy STAT rates. */
#ifndef LMB_TURN_OBSERVATION_H
#define LMB_TURN_OBSERVATION_H
#include "lumabri_calibration.h"
#include "lumabri_metrics.h"
#include "lumabri_stage_metrics.h"

static inline int lmb_turn_observation(LmbCalibration *out, const LmbCalibration *seed,
    const LmbCalKey *key, const char *stat, uint32_t abi, const char *numeric_class,
    double ttft, double measured_at, uint32_t source) {
    LmbGenerationMetrics metrics;
    if (!out || !seed || !key || !abi || !numeric_class || !numeric_class[0] ||
        strnlen(numeric_class,97)>=97 || !stat || strncmp(stat,"STAT ",5) ||
        lmb_metrics_parse(stat,&metrics) || lmb_metrics_decode_rate(&metrics)<=0) return -1;
    const char *p=stat+5; uint32_t generated=0,prompt=0;
    /* Parse the five leading fields without scanf integer overflow. */
    for (unsigned i=0;i<5;i++) {
        while (*p==' ') p++;
        if (*p<'0' || *p>'9') return -1;
        errno=0; char *end;
        if (i==0 || i==4) {
            unsigned long n=strtoul(p,&end,10);
            if (errno || !n || n>1048576) return -1;
            if (i==0) generated=(uint32_t)n; else prompt=(uint32_t)n;
        } else {
            double n=strtod(p,&end); if (errno || !isfinite(n) || n<0) return -1;
        }
        if (*end!=' ') return -1;
        p=end+1;
    }
    if (generated!=metrics.generated_tokens || prompt>key->context) return -1;
    LmbCalibration record=*seed;
    record.key=*key; record.key.adapter_abi=abi;
    snprintf(record.key.numeric_class,sizeof record.key.numeric_class,"%s",numeric_class);
    record.decode_tok_s=lmb_metrics_decode_rate(&metrics);
    record.ttft_seconds=ttft; record.measured_at=measured_at; record.source=source;
    record.prompt_tokens=prompt; record.generated_tokens=generated;
    record.samples=1; record.stage_count=0; record.link_count=0;
    memset(record.stage_decode_seconds,0,sizeof record.stage_decode_seconds);
    memset(record.links,0,sizeof record.links);
    if (!lmb_links_parse(stat,record.key.nodes,record.key.layer_begin,record.key.layer_end,record.links))
        record.link_count=record.key.nodes;
    LmbStageSample stages[LMB_STAGE_PROFILE_MAX]; uint32_t count=0;
    if (!lmb_stage_samples_parse(stat,stages,&count) && count==record.key.nodes) {
        int valid=1;
        for (uint32_t i=0;i<count;i++) {
            if (stages[i].begin!=record.key.layer_begin[i] || stages[i].end!=record.key.layer_end[i] ||
                stages[i].calls!=metrics.decode_steps) valid=0;
            record.stage_decode_seconds[i]=stages[i].seconds/stages[i].calls;
        }
        if (valid) record.stage_count=count;
    }
    lmb_cal_observation_count(&record,seed);
    if (!lmb_cal_valid(&record)) return -1;
    *out=record; return 0;
}
#endif
