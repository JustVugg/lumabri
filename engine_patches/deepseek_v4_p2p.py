#!/usr/bin/env python3
# Patch colibri's multi-file deepseek_v4.c for the lumabri chatter.
#
# The single-file deepseek.c had a hand-written .diff; deepseek_v4.c is the new
# unit-amalgamated source, compiled once per COLI_V4_UNIT_*, so a line diff is
# brittle and the lumabri client cannot be #included directly (its static state
# would be duplicated per unit). Instead we anchor on the three expert-apply
# sites, call thin extern bridge functions (defined once in lumi_v4_bridge.c),
# and route every routed expert to peers when a swarm covers them.
#
# Anchors mirror the old patch exactly: before each expert loop, skip it and
# apply the whole layer remotely. Idempotent and self-checking: it counts the
# insertions and fails loudly if the source layout moved.
import sys, re

def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = open(src).read()
    n = 0
    # The bridge externs are forced in with -include lumi_v4_ext.h, so they sit
    # at file scope for every COLI_V4_UNIT_* section; this script only inserts
    # the call hooks.

    # apply hooks: (anchor, hook, expected count)
    hooks = [
        # site 1 — moe_token: single loader, reset n
        ('    if (!result) memset(output, 0, (size_t)d * sizeof(*output));\n'
         '    for (int expert_id = 0; !result && expert_id < n; expert_id++) {',
         '    if (!result) memset(output, 0, (size_t)d * sizeof(*output));\n'
         '#ifdef LUMABRI_P2P\n'
         '    if (!result && lumi_v4_bridge_on(weights->plan.layer)) {\n'
         '        lumi_v4_bridge_apply(weights->plan.layer, indices, route_weights,\n'
         '                             topk, input, 1, d, output);\n'
         '        n = 0;\n'
         '    }\n'
         '#endif\n'
         '    for (int expert_id = 0; !result && expert_id < n; expert_id++) {'),
        # site 2 — moe_token_pipeline: dual loader, reset selected.
        # Current colibri allocates a `views` array (and, under COLI_V4_GPU_TIER,
        # a batch path) before the per-expert loop, so the anchor is the malloc,
        # not the loop. selected=0 stays correct on the CPU build we compile:
        # the GPU batch path is #ifdef'd out, so the only tail that runs is
        # `output = round(output + shared)` — identical to sites 1 and 3, with
        # the routed partial already written remotely. (views = malloc(0) is a
        # valid non-NULL pointer on glibc; selected==topk>0 in every local run.)
        ('    if (!result) memset(output, 0, (size_t)d * sizeof(*output));\n'
         '\n'
         '#ifdef COLI_V4_EXPERIMENTAL_DUAL_EXPERT_LOADER\n'
         '    ColiExpertView *views = malloc((size_t)selected * sizeof(*views));',
         '    if (!result) memset(output, 0, (size_t)d * sizeof(*output));\n'
         '#ifdef LUMABRI_P2P\n'
         '    if (!result && lumi_v4_bridge_on(weights->plan.layer)) {\n'
         '        lumi_v4_bridge_apply(weights->plan.layer, indices, route_weights,\n'
         '                             topk, input, 1, d, output);\n'
         '        selected = 0;\n'
         '    }\n'
         '#endif\n'
         '\n'
         '#ifdef COLI_V4_EXPERIMENTAL_DUAL_EXPERT_LOADER\n'
         '    ColiExpertView *views = malloc((size_t)selected * sizeof(*views));'),
        # site 3 — v4_moe_batch_union: batch, reset n
        ('    if (!result)\n'
         '        memset(outputs, 0, (size_t)batch * d * sizeof(*outputs));\n'
         '\n'
         '    int key_count = 0;',
         '    if (!result)\n'
         '        memset(outputs, 0, (size_t)batch * d * sizeof(*outputs));\n'
         '#ifdef LUMABRI_P2P\n'
         '    if (!result && lumi_v4_bridge_on(weights->plan.layer)) {\n'
         '        lumi_v4_bridge_apply(weights->plan.layer, indices, route_weights,\n'
         '                             topk, inputs, batch, d, outputs);\n'
         '        n = 0;\n'
         '    }\n'
         '#endif\n'
         '\n'
         '    int key_count = 0;'),
    ]
    for anchor, repl in hooks:
        if s.count(anchor) != 1:
            sys.exit("deepseek_v4.c: expert-apply anchor found %d times (want 1) — "
                     "layout changed" % s.count(anchor))
        s = s.replace(anchor, repl, 1); n += 1

    # init hook — the always-run tail of coli_v4_engine_open (4-space indent,
    # not the conditional 8-space one), so it fires on every successful open.
    init_anchor = "\n    engine->summary.head_resident = engine->head_cache.data != NULL;\n"
    inits = s.count(init_anchor)
    if inits < 1:
        sys.exit("deepseek_v4.c: engine-init anchor not found — layout changed")
    init_hook = (init_anchor +
        "#ifdef LUMABRI_P2P\n"
        "    lumi_v4_bridge_init(engine->config.num_hidden_layers,\n"
        "                        engine->config.n_routed_experts,\n"
        "                        engine->config.hidden_size);\n"
        "    atexit(lumi_v4_bridge_report);\n"
        "#endif\n")
    s = s.replace(init_anchor, init_hook); n += inits

    open(dst, "w").write(s)
    if n < 4:
        sys.exit("expected at least 4 insertions, made %d" % n)
    print("deepseek_v4 p2p: %d hooks inserted (%d engine-open tail)" % (n, inits))

if __name__ == "__main__":
    main()
