/* Compile the original upstream monolithic engine independently of our
 * generated Edge/Segment copies. Read the same converted tensor payloads. */
#define QWEN36_NO_MAIN
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "qwen36.c"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

int main(int argc, char **argv) {
    if (argc!=3) return 2;
    FILE *out=fopen(argv[2],"w"); if (!out) return 2;
    Model m;
    g_q36_kv16=0; g_pilot=0;
    model_init(&m,argv[1],4096,8);
    int probe=0;
    while(probe+1<m.c.n_layers && q36_dense_l(&m.c,probe)) probe++;
    m.quant_bits=container_layer_is_int4(&m,probe) ? 4 : 8;
    m.max_t=128; ensure_kv(&m);
    int ids[40]={1,2,3,4,5,6,7,8};
    for (int pos=8;pos<40;pos++) {
        float *logits=step(&m,pos==8 ? ids : &ids[pos-1],pos==8 ? 8 : 1,pos==8 ? 0 : pos-1);
        if (!logits) return 1;
        int best=0;
        for (int t=0;t<m.c.vocab;t++) {
            if (!isfinite(logits[t])) return 1;
            if (logits[t]>logits[best]) best=t;
        }
        ids[pos]=best;free(logits);
    }
    fprintf(out,"{\"prompt_ids\":[1,2,3,4,5,6,7,8],\"full_ids\":[");
    for(int i=0;i<40;i++) fprintf(out,"%s%d",i ? "," : "",ids[i]);
    fprintf(out,"]}\n");
    return fclose(out) ? 1 : 0;
}
