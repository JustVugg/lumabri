#include "lumabri_planner.h"
#include "planner_adapters/gguf_model.h"
#include "src/planner/lumabri_gguf_metadata.h"
int main(int argc,char **argv) {
    if(argc!=2 && argc!=3)return 2;
    if(argc==3 && !strcmp(argv[1],"--metadata")) {
        LmbGguf g={0};LmbGgufMetadata m={0};
        if(lmb_gguf_model(argv[2],&g))return 1;
        int rc=lmb_gguf_metadata(&g,&m);
        if(!rc)printf("{\"config\":%s,\"tokenizer\":%s,\"tokenizer_config\":%s,\"generation_config\":%s}\n",
            m.config.data,m.tokenizer.data,m.tokenizer_config.data,m.generation_config.data);
        lmb_gguf_metadata_free(&m);lmb_gguf_free(&g);return rc?1:0;
    }
    if(argc==3 && !strcmp(argv[1],"--boundary")) {
        LmbModelShape m;
        if(lmb_shape_from_config(argv[2],&m) || !m.sizing_verified)return 1;
        printf("%u\n",m.boundary_width?m.boundary_width:m.hidden);return 0;
    }
    if(argc==3 && !strcmp(argv[1],"--cost")) {
        LmbModelShape m;
        if(lmb_shape_from_config(argv[2],&m) || !m.sizing_verified)return 1;
        for(uint32_t a=0;a<m.layers;a++)for(uint32_t b=a+1;b<=m.layers;b++) {
            LmbRangeCost c=lmb_estimate_segment(&m,a,b,128,1);
            if(!c.ok)return 1;
            printf("%u %u %llu %llu %llu\n",a,b,(unsigned long long)c.resident_bytes,
                (unsigned long long)c.state_bytes,(unsigned long long)c.scratch_bytes);
        }
        return 0;
    }
    LmbGguf g={0};int rc;
    if(argc==3 && !strcmp(argv[1],"--model")) rc=lmb_gguf_model(argv[2],&g);
    else {
        int fd=lmb_plan_open(argv[1]);if(fd<0)return 3;
        rc=lmb_gguf_parse(fd,&g);close(fd);
    }
    if(rc)return 1;
    const char *arch=lmb_gguf_text(lmb_gguf_get(&g,"general.architecture"));
    printf("%s %u %u %llu\n",arch ? arch : "unknown",g.nvalues,g.ntensors,(unsigned long long)g.header_bytes);
    for(uint32_t i=0;i<g.ntensors;i++) printf("%s %u %llu %llu\n",g.tensors[i].name,g.tensors[i].type,
        (unsigned long long)g.tensors[i].offset,(unsigned long long)g.tensors[i].bytes);
    lmb_gguf_free(&g);return 0;
}
