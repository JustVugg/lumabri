#define _GNU_SOURCE
#undef NDEBUG
#include <assert.h>
#include "lumabri_planner.h"
#include "planner_adapters/deepseek_v41.h"
int main(int argc,char **argv) {
    assert(argc==2);
    uint64_t n;char *cfg=lmb_v41_read_json(argv[1],"config.json",65535,&n);assert(cfg);
    const char *text=lmb_json_member(cfg,"text_config");if(!text)text=cfg;
    LmbModelShape m={0};
    int rc=lmb_v41_memory(argv[1],text,&m);
    if(rc){fprintf(stderr,"V41 memory inspection refused fixture\n");return 1;}
    m.sizing_verified=1;
    LmbRangeCost whole=lmb_estimate_segment(&m,0,m.layers,96,1);
    LmbRangeCost small=lmb_estimate_segment(&m,0,1,96,1);
    LmbRangeCost longer=lmb_estimate_segment(&m,0,m.layers,128,1);
    LmbRangeCost two=lmb_estimate_segment(&m,0,m.layers,96,2);
    assert(whole.ok && small.ok && whole.resident_bytes>small.resident_bytes);
    assert(longer.state_bytes>whole.state_bytes && two.state_bytes==whole.state_bytes*2);
    assert(!lmb_estimate_segment(&m,0,m.layers,m.max_context+1,1).ok);
    assert(!m.disk_streaming && m.boundary_width>m.hidden);
    printf("V41 memory: weights=%llu state=%llu scratch=%llu edge=%llu boundary=%u\n",
        (unsigned long long)whole.resident_bytes,(unsigned long long)whole.state_bytes,
        (unsigned long long)whole.scratch_bytes,(unsigned long long)m.edge_resident_bytes,m.boundary_width);
    free(cfg);return 0;
}
