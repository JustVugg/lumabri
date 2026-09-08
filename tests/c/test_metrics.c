#include "lumabri_metrics.h"
#include <assert.h>

int main(void) {
    LmbGenerationMetrics m={.generated_tokens=9,.decode_steps=8,
        .prefill_seconds=15,.decode_seconds=2,.total_seconds=17.2}, parsed;
    assert(lmb_metrics_decode_rate(&m)==4);
    char extension[192], stat[256];
    assert(!lmb_metrics_format(&m,extension,sizeof extension));
    snprintf(stat,sizeof stat,"STAT 9 4 0 0 20 0 %s\n",extension);
    assert(!lmb_metrics_parse(stat,&parsed) && parsed.generated_tokens==9 &&
           parsed.decode_steps==8 && lmb_metrics_decode_rate(&parsed)==4);
    assert(lmb_metrics_parse("STAT 9 0.5 0 0",&parsed)==1);
    const char *bad[]={" PERF2 9 8 15 2 17.2", " PERF1 -1 0 0 0 0",
        " PERF1 4294967297 0 0 0 0", " PERF1 9 -8 15 2 17.2",
        " PERF1 9 9 15 2 17.2", " PERF1 9 8 nan 2 17.2",
        " PERF1 9 8 15 inf 17.2", " PERF1 9 8 15 -2 17.2",
        " PERF1 9 8 15 2 16", " PERF1 9 8 15 2 17.2 trailing",
        " PERF1 1 0 1 0.1 1.1", " PERF1 0 0 0 0 0", " PERF1 9 8 15 2"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) assert(lmb_metrics_parse(bad[i],&parsed)==-1);
    m=(LmbGenerationMetrics){.generated_tokens=1,.prefill_seconds=1,.total_seconds=1.1};
    assert(lmb_metrics_valid(&m) && lmb_metrics_decode_rate(&m)==0);
    assert(!lmb_metrics_format(&m,extension,sizeof extension));
    assert(lmb_metrics_format(&m,extension,4)==-1);
    m.decode_seconds=NAN; assert(!lmb_metrics_valid(&m));
    puts("GENERATION METRICS: PASS (prefill separate, first token excluded, bounded wire format)");
    return 0;
}
