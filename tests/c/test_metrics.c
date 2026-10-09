#include "lumabri_metrics.h"
#include "lumabri_stage_metrics.h"
#include "src/runtime/lumabri_response_metrics.h"
#include <assert.h>
#include <float.h>

static void response_timings(void) {
    LmbResponseMetrics m={.started=10}; LmbResponseSummary s;
    const char *stat="STAT 4 4 0 0 2 0 PERF1 4 3 1 0.75 1.75";
    lmb_response_progress(&m,"PROGRESS req PREFILL 1 2",10.5);
    lmb_response_progress(&m,"PROGRESS req DECODE 1 4",11);
    lmb_response_text(&m,11.125,2);
    lmb_response_progress(&m,"PROGRESS req DECODE 2 4",11.5);
    lmb_response_progress(&m,"PROGRESS req DECODE 3 4",11.5); /* same read, zero observed gap */
    lmb_response_progress(&m,"PROGRESS req DECODE 4 4",11.75);
    lmb_response_text(&m,11.75,1);
    assert(!lmb_response_summary(&m,stat,1,12,12.5,&s));
    assert(s.text_known && s.ttft_seconds==1.125 && s.generation_seconds==2 && s.completion_seconds==2.5);
    assert(s.token_times_known && s.tokens==4 && s.token_first_seconds==1);
    assert(s.quantiles_known && s.gaps==3 && s.gap_p50_seconds==.25 && s.gap_p95_seconds==.5);
    assert(!lmb_response_summary(&m,stat,2,12,12.5,&s) && s.text_known && !s.token_times_known && !s.quantiles_known);
    assert(!lmb_response_summary(&m,"STAT 4 4 0 0",1,12,12.5,&s) && !s.token_times_known);
    assert(lmb_response_summary(&m,stat,0,12,12.5,&s));
    assert(lmb_response_summary(&m,stat,1,9,12.5,&s));
    assert(lmb_response_summary(&m,stat,1,12,11,&s));
    assert(lmb_response_summary(&m,stat,1,NAN,12,&s));
    const char *bad[]={"DECODE", "DECODE 0", "DECODE 2", "DECODE 1 0", "DECODE -1", "DECODE 1 4 junk",
        "DECODE 4294967297", "DECODE 1 1048577", "DECODE 1.0", "DECODE 1 4 4"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
        m=(LmbResponseMetrics){.started=10}; char line[100]; snprintf(line,sizeof line,"PROGRESS req %s",bad[i]);
        lmb_response_progress(&m,line,11); assert(m.invalid);
        lmb_response_text(&m,11,1);
        assert(!lmb_response_summary(&m,stat,1,12,12,&s) && s.text_known && !s.token_times_known);
    }
    m=(LmbResponseMetrics){.started=10};
    lmb_response_progress(&m,"PROGRESS req DECODE 1",11);
    assert(!m.invalid && m.tokens==1); /* legacy optional total */
    assert(!lmb_response_summary(&m,"STAT 1 PERF1 1 0 1 0 1",1,11,11,&s));
    assert(s.token_times_known && !s.quantiles_known && !s.text_known);
    lmb_response_progress(&m,"PROGRESS req DECODE 1",11); assert(m.invalid);
    m=(LmbResponseMetrics){.started=10};
    lmb_response_progress(&m,"PROGRESS req DECODE 1",9); assert(m.invalid);
    m=(LmbResponseMetrics){.started=10};
    lmb_response_progress(&m,"PROGRESS req DECODE 1",INFINITY); assert(m.invalid);
    m=(LmbResponseMetrics){.started=0};
    for (unsigned i=1;i<=LMB_RESPONSE_GAPS+2;i++) {
        char line[80]; snprintf(line,sizeof line,"PROGRESS req DECODE %u",i);
        lmb_response_progress(&m,line,i);
    }
    char large[120]; snprintf(large,sizeof large,"STAT 1 PERF1 %u %u 1 %u %u",
        m.tokens,m.tokens-1,m.tokens-1,m.tokens);
    assert(!m.invalid && m.gap_count==LMB_RESPONSE_GAPS);
    assert(!lmb_response_summary(&m,large,1,m.tokens,m.tokens,&s) && s.token_times_known && !s.quantiles_known);
    m=(LmbResponseMetrics){.started=10}; lmb_response_text(&m,11,0);
    assert(!lmb_response_summary(&m,stat,1,12,12,&s) && !s.text_known && !s.token_times_known);
}

int main(void) {
    response_timings();
    puts("RESPONSE TIMING: PASS (visible text vs token notifications, exact bounded quantiles, missing telemetry, replay exclusion)");
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
