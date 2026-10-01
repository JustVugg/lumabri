#include "lumabri_metrics.h"
#include "lumabri_stage_metrics.h"
#include <assert.h>
#include <float.h>

int main(void) {
    LmbStageSample samples[LMB_STAGE_PROFILE_MAX] = {{0}};
    uint32_t count = 99;
    const char *profile = "STAT 9 4 0 0 20 0 STAGES1 2 0 12 8 0.8 12 16 8 1.2 PERF1 9 8 15 2 17.2";
    assert(!lmb_stage_samples_parse(profile, samples, &count) && count == 2);
    assert(samples[0].begin == 0 && samples[0].end == 12 && samples[0].calls == 8 &&
           samples[0].seconds == .8 && samples[1].seconds == 1.2);
    LmbGenerationMetrics compatible;
    assert(!lmb_metrics_parse(profile, &compatible) && lmb_metrics_decode_rate(&compatible) == 4);
    assert(lmb_stage_samples_parse("STAT 9 4 PERF1 9 8 15 2 17.2", samples, &count) == 1 && !count);
    const char *invalid_profiles[] = {
        " STAGES1 0 PERF1 ", " STAGES1 33 PERF1 ", " STAGES1 -1 PERF1 ",
        " STAGES1 1 1 2 8 1 PERF1 ", " STAGES1 1 0 0 8 1 PERF1 ",
        " STAGES1 1 0 2 0 1 PERF1 ", " STAGES1 1 0 2 8 nan PERF1 ",
        " STAGES1 1 0 2 8 inf PERF1 ", " STAGES1 1 0 2 8 -1 PERF1 ",
        " STAGES1 1 0 2 8 0 PERF1 ", " STAGES1 1 0 2 8 1e100 PERF1 ",
        " STAGES1 1 0 2 8 1 ", " STAGES1 1 0 2 8 1", " STAGES1 1 ",
        " STAGES1 2 0 12 8 1 13 16 8 1 PERF1 ",
        " STAGES1 1 0 4294967296 8 1 PERF1 "
    };
    for (size_t i = 0; i < sizeof invalid_profiles / sizeof *invalid_profiles; i++) {
        LmbStageSample previous = samples[0];
        count = 99;
        assert(lmb_stage_samples_parse(invalid_profiles[i], samples, &count) == -1 && !count);
        assert(!memcmp(&samples[0], &previous, sizeof previous));
    }
    char maximum[4096]; size_t at = (size_t)snprintf(maximum, sizeof maximum, "STAT 9 4 0 0 20 0 STAGES1 32");
    for (unsigned i = 0; i < 32; i++)
        at += (size_t)snprintf(maximum + at, sizeof maximum - at, " %u %u 8 0.125000000", i, i + 1);
    assert(at > 512 && at + 40 < sizeof maximum);
    snprintf(maximum + at, sizeof maximum - at, " PERF1 9 8 15 4 19.2");
    assert(!lmb_stage_samples_parse(maximum, samples, &count) && count == 32 && samples[31].end == 32);
    assert(!lmb_metrics_parse(maximum, &compatible));
    LmbStageMetrics stage = {0};
    assert(!lmb_stage_metrics_add(&stage, 0, 8, .25));
    assert(!lmb_stage_metrics_add(&stage, 0, 1, .125)); /* one-row PREFILL, not decode */
    assert(!stage.decode_calls && stage.prefill_calls == 2 && stage.prefill_rows == 9);
    assert(!lmb_stage_metrics_add(&stage, 1, 1, .125));
    assert(!lmb_stage_metrics_add(&stage, 1, 1, .5));
    assert(!lmb_stage_metrics_add(&stage, 1, 1, 0));
    assert(stage.decode_calls == 3 && stage.decode_seconds == .625 &&
           stage.decode_min_seconds == 0 && stage.decode_max_seconds == .5 &&
           stage.prefill_seconds == .375);
    LmbStageMetrics saved = stage;
    assert(lmb_stage_metrics_add(&stage, 1, 2, .25));
    assert(lmb_stage_metrics_add(&stage, 0, 0, .25));
    assert(lmb_stage_metrics_add(&stage, 2, 1, .25));
    assert(lmb_stage_metrics_add(&stage, 0, 1, -1));
    assert(lmb_stage_metrics_add(&stage, 0, 1, NAN));
    assert(lmb_stage_metrics_add(&stage, 1, 1, INFINITY));
    assert(lmb_stage_metrics_add(NULL, 1, 1, .25));
    assert(!memcmp(&stage, &saved, sizeof stage));
    stage.decode_calls = UINT64_MAX;
    assert(lmb_stage_metrics_add(&stage, 1, 1, 0));
    stage = saved; stage.prefill_calls = UINT64_MAX;
    assert(lmb_stage_metrics_add(&stage, 0, 1, 0));
    stage = saved; stage.prefill_rows = UINT64_MAX;
    assert(lmb_stage_metrics_add(&stage, 0, 1, 0));
    stage = saved; stage.decode_seconds = DBL_MAX;
    assert(lmb_stage_metrics_add(&stage, 1, 1, DBL_MAX));
    assert(stage.decode_seconds == DBL_MAX && stage.decode_calls == saved.decode_calls);
    stage = saved; stage.prefill_seconds = DBL_MAX;
    assert(lmb_stage_metrics_add(&stage, 0, 1, DBL_MAX));
    LmbGenerationMetrics m={.generated_tokens=9,.decode_steps=8,
        .prefill_seconds=15,.decode_seconds=2,.total_seconds=17.2}, parsed;
    assert(lmb_metrics_decode_rate(&m)==4);
    char extension[192], stat[256];
    assert(!lmb_metrics_format(&m,extension,sizeof extension));
    snprintf(stat,sizeof stat,"STAT 9 4 0 0 20 0 %s\n",extension);
    assert(!lmb_metrics_parse(stat,&parsed) && parsed.generated_tokens==9 &&
           parsed.decode_steps==8 && lmb_metrics_decode_rate(&parsed)==4);
    assert(lmb_metrics_parse("STAT 9 0.5 0 0",&parsed)==1);
    const char *bad[]={" PERF_UNAVAILABLE", " PERF2 9 8 15 2 17.2", " PERF1 -1 0 0 0 0",
        " PERF1 4294967297 0 0 0 0", " PERF1 9 -8 15 2 17.2",
        " PERF1 9 9 15 2 17.2", " PERF1 9 8 nan 2 17.2",
        " PERF1 9 8 15 inf 17.2", " PERF1 9 8 15 -2 17.2",
        " PERF1 9 8 15 2 16", " PERF1 9 8 15 2 17.2 trailing",
        " PERF1 1 0 1 0.1 1.1", " PERF1 0 0 0 0 0", " PERF1 9 8 15 2"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) {
        parsed=m;
        assert(lmb_metrics_parse(bad[i],&parsed)==-1);
        assert(!parsed.generated_tokens && !parsed.decode_steps &&
               !parsed.prefill_seconds && !parsed.decode_seconds && !parsed.total_seconds);
    }
    assert(lmb_metrics_parse(NULL,&parsed)==-1);
    assert(lmb_metrics_parse("STAT 1 0 0 0",NULL)==-1);
    m=(LmbGenerationMetrics){.generated_tokens=1,.prefill_seconds=1,.total_seconds=1.1};
    assert(lmb_metrics_valid(&m) && lmb_metrics_decode_rate(&m)==0);
    assert(!lmb_metrics_format(&m,extension,sizeof extension));
    assert(lmb_metrics_format(&m,extension,4)==-1);
    m.decode_seconds=NAN; assert(!lmb_metrics_valid(&m));
    puts("GENERATION METRICS: PASS (prefill separate, first token excluded, bounded wire format)");
    return 0;
}
