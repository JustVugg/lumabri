/* One home for the lumabri client state, so the multi-file deepseek_v4 chatter
 * links a single L instead of one per compilation unit. deepseek_v4_p2p.py
 * gives the units extern declarations of these four; here are the definitions.
 * Generated/placed by the Makefile; safe to regenerate. */
#define LUMABRI_P2P
#define LUMIBRI_P2P
#include "lumabri_client.h"
#include <sys/resource.h>
#include <dlfcn.h>
#include "lumi_v4_ext.h"

static void *home_expert_context;
static LmbHomeExpertFn home_expert_function;
void lmb_home_expert_provider(void *ctx, LmbHomeExpertFn fn) {
    home_expert_context = ctx; home_expert_function = fn;
}
int lmb_home_expert_available(void) { return home_expert_function != NULL; }
int lmb_home_expert_apply(int layer, int expert, const float *x, int D, float *out) {
    return home_expert_function ? home_expert_function(home_expert_context, layer, expert, x, D, out) : -1;
}

static int resident_prepared;
void lmb_resident_adapter_prepared(void) { resident_prepared = 1; }
int lmb_resident_adapter_is_prepared(void) { return resident_prepared; }
int lmb_resident_budget_exceeded(unsigned long long limit) {
    struct rusage use;
    if (!limit || getrusage(RUSAGE_SELF, &use)) return 1;
#ifdef __APPLE__
    return (unsigned long long)use.ru_maxrss > limit;
#else
    return (unsigned long long)use.ru_maxrss * 1024u > limit;
#endif
}

int lmb_resident_retain(int fd, unsigned long long offset,
                        unsigned long long length, unsigned long long limit) {
    if (!length || length >= limit || lmb_resident_budget_exceeded(limit - length)) return -1;
    int (*retain)(int, uint64_t, uint64_t) =
        (int (*)(int, uint64_t, uint64_t))dlsym(RTLD_DEFAULT, "lmb_weights_retain");
    return !retain || retain(fd, offset, length) || lmb_resident_budget_exceeded(limit) ? -1 : 0;
}

void lumi_v4_bridge_init(int n_layers, int n_experts, int hidden) {
    static int done = 0;                 /* several engine-open paths may call */
    if (done) return;
    done = 1;
    lumi_init_ex(n_layers, n_experts, hidden, NULL);   /* every V4 layer routes */
}
int lumi_v4_bridge_on(int layer) { return lumi_layer_on(layer); }
int lumi_v4_bridge_apply(int layer, const int *indices, const float *weights,
                         int topk, const float *x, int batch, int D, float *out) {
    return lumi_moe_apply_v4(layer, indices, weights, topk, x, batch, D, out);
}
void lumi_v4_bridge_report(void) { lumi_report(); }
