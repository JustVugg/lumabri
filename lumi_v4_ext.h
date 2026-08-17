/* Extern declarations of the lumabri bridge, forced into every deepseek_v4
 * unit with -include so they sit at true file scope regardless of which
 * COLI_V4_UNIT_* section is active. The definitions live once in
 * lumi_v4_bridge.c; the hooks inserted by deepseek_v4_p2p.py call these. */
#ifndef LUMI_V4_EXT_H
#define LUMI_V4_EXT_H
#ifdef LUMABRI_P2P
extern void lumi_v4_bridge_init(int n_layers, int n_experts, int hidden);
extern int  lumi_v4_bridge_on(int layer);
extern void lumi_v4_bridge_apply(int layer, const int *indices,
        const float *weights, int topk, const float *x, int batch,
        int D, float *out);
extern void lumi_v4_bridge_report(void);
#endif
#endif
