/* One home for the lumabri client state, so the multi-file deepseek_v4 chatter
 * links a single L instead of one per compilation unit. deepseek_v4_p2p.py
 * gives the units extern declarations of these four; here are the definitions.
 * Generated/placed by the Makefile; safe to regenerate. */
#define LUMABRI_P2P
#define LUMIBRI_P2P
#include "lumabri_client.h"

void lumi_v4_bridge_init(int n_layers, int n_experts, int hidden) {
    static int done = 0;                 /* several engine-open paths may call */
    if (done) return;
    done = 1;
    lumi_init_ex(n_layers, n_experts, hidden, NULL);   /* every V4 layer routes */
}
int lumi_v4_bridge_on(int layer) { return lumi_layer_on(layer); }
void lumi_v4_bridge_apply(int layer, const int *indices, const float *weights,
                          int topk, const float *x, int batch, int D, float *out) {
    lumi_moe_apply_v4(layer, indices, weights, topk, x, batch, D, out);
}
void lumi_v4_bridge_report(void) { lumi_report(); }
