#!/usr/bin/env python3
"""Generate the lumabri P2P patches for the colibri engines.

The engines are never modified in place. This reads a colibri checkout,
applies each engine's hooks to a copy, and writes a unified diff into this
directory. Anchors are exact source strings, so if upstream moves the code
the generator fails loudly instead of producing a patch that applies to the
wrong place.

    python3 engine_patches/make_patches.py [--engine-dir ../moe-stream/c]
                                           [--check]

--check regenerates into a temporary dir and diffs against what is committed,
so CI (or a human) can tell whether the patches still match the engines.

The hooks themselves are four, and the same four everywhere:

  1. include the client next to the engine's own includes;
  2. after the model is loaded, tell the client the model's shape — how many
     layer slots can hold experts, how many experts, the hidden size, and
     which slots actually route (dense layers and an absent MTP row do not);
  3. in the MoE function, once routing has chosen the experts, hand them to
     the swarm and skip the local resolve/compute — the routing, the weighted
     sum and the shared expert stay on the chatter, because those are the
     model's semantics rather than the peer's business;
  4. report on exit.

Everything is inert without -DLUMIBRI_P2P, and inert at runtime unless a
tracker or LUMABRI_EXPERTS says otherwise.
"""
import argparse, difflib, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

INCLUDE = """
#ifdef LUMIBRI_P2P
/* lumabri: with a tracker (or LUMABRI_EXPERTS) the routed experts run on
 * peers instead of being read from disk. Inert without the define, and inert
 * at runtime unless the swarm can cover every expert. */
#include "lumibri_client.h"
#endif
"""


def hook(anchor, add, where="after", count=1):
    return {"anchor": anchor, "add": add, "where": where, "count": count}


# --------------------------------------------------------------------------
# olmoe: every layer routes, no shared expert, no MTP row.
# --------------------------------------------------------------------------
OLMOE = [
    hook('#include "route_trace.h"                    /* shared routing telemetry (#700) */\n',
         INCLUDE),
    hook("    m->dense_load_s = now_s() - t0;\n", """#ifdef LUMIBRI_P2P
    lumi_init(c->n_layers, c->n_experts, c->hidden);
    atexit(lumi_report);
#endif
"""),
    hook("        const float *xs = x + (int64_t)s*D;\n", """#ifdef LUMIBRI_P2P
        if (lumi_layer_on(layer)) {   /* the K experts run on peers; routing and the sum stay here */
            lumi_moe_apply(layer, idx, val, K, xs, D, out + (int64_t)s*D);
            continue;
        }
#endif
"""),
]

# --------------------------------------------------------------------------
# colibri (GLM): dense layers up front, an MTP row at index n_layers, and a
# shared expert that must keep running locally. The hook lands after the
# batch union so `nu = 0` is all it takes to skip the local compute.
# --------------------------------------------------------------------------
COLIBRI = [
    hook('#include "route_trace.h"                           /* ROUTE_TRACE + .coli_usage, engine-agnostic (#700) */\n',
         INCLUDE),
    hook("    m->resident_bytes=rb;\n", """#ifdef LUMIBRI_P2P
    {   /* which slots route: dense layers hold no experts, and the MTP row
         * at index n_layers only exists when the model has one. Same rule as
         * the tier fill below. */
        int nl=c->n_layers, slots=nl+1;
        unsigned char *routed=calloc((size_t)slots,1);
        for(int li=0;li<slots;li++)
            routed[li]=(li<nl&&m->L[li].sparse)||(li==nl&&m->has_mtp);
        lumi_init_ex(slots, c->n_experts, c->hidden, routed);
        free(routed);
        atexit(lumi_report);
    }
#endif
"""),
    hook("""    for(int s=0;s<S;s++) for(int kk=0;kk<keff[s];kk++){
        int e=idxs[(int64_t)s*K+kk];
        if(!seen[e]){ seen[e]=1; uniq[nu++]=e; }
    }
""", """#ifdef LUMIBRI_P2P
    if(lumi_layer_on(layer)){
        /* The routed experts run on peers, batched by expert exactly as the
         * loop below would batch them — anything else is different floats.
         * Routing (above), the weighted sum and FASE E's shared expert stay
         * here; emptying the union is what makes FASE C/D find nothing left
         * to resolve or compute. */
        lumi_moe_apply_batch(layer, idxs, ws, keff, K, x, S, D, out);
        nu=0;
    }
#endif
"""),
]

# --------------------------------------------------------------------------
# inkling: dense-first layers too, gate and up fused in one 2I-row matmul.
# The hook sits in the compute pass, which is already per (position, k).
# --------------------------------------------------------------------------
INKLING = [
    hook("static float sigmoidf(float x) { return 1.f / (1.f + expf(-x)); }\n",
         INCLUDE, where="before"),
    hook("        m->euse += keff[s];\n    }\n", """#ifdef LUMIBRI_P2P
    /* The routed experts run on peers — all K of a position issued together,
     * so the cost is one round trip per layer and not per expert. The shared
     * experts below, the routing above and the weighted sum stay here. */
    int lumi_remote = lumi_layer_on(layer);
    if (lumi_remote)
        for (int s = 0; s < S; s++)
            lumi_moe_apply(layer, idx + (int64_t)s*K, wgt + (int64_t)s*(K+ns),
                           keff[s], x + (int64_t)s*D, D, out + (int64_t)s*D);
#endif
"""),
    hook("    m->eusage = rt_counts_all();                  /* alias: the bump sites stay as they are */\n",
         """#ifdef LUMIBRI_P2P
    {   /* dense layers hold no experts: without the mask their non-existent
         * ones count as missing and phase 2 never turns on */
        unsigned char *routed = calloc((size_t)c->n_layers, 1);
        for (int i = 0; i < c->n_layers; i++) routed[i] = c->sparse[i];
        lumi_init_ex(c->n_layers, E, c->hidden, routed);
        free(routed);
        atexit(lumi_report);
    }
#endif
"""),
    hook("    int64_t npair = (int64_t)S*K;\n", """#ifdef LUMIBRI_P2P
    if (lumi_remote) npair = 0;       /* nothing left to acquire, fill or compute */
#endif
"""),
]

# --------------------------------------------------------------------------
# kimi_k3: the experts live in the LATENT space (c->latent, not hidden) and
# the engine has already built the union by the time it calls
# experts_apply_union — so the hook hands that union straight over and leaves
# nothing behind. The latent projections above and below, and the shared
# experts, stay local.
# --------------------------------------------------------------------------
KIMI = [
    hook("static inline float sigmoidf_(float x){ return 1.f/(1.f+expf(-x)); }\n",
         INCLUDE, where="before"),
    hook("""    fprintf(stderr,"[K3] init done in %.1fs | %d layers | expert cache %d/layer (%.1f MB/slot) | RSS %.1f GB\\n",
            now_s()-t0,c->n_layers,cap,m->e_slot/1e6,rss_gb());
""", """#ifdef LUMIBRI_P2P
    {   /* the experts live in the LATENT space, so that — not hidden — is the
         * width the peers must agree on. Dense layers route nothing. */
        unsigned char *routed=calloc((size_t)c->n_layers,1);
        for(int i=0;i<c->n_layers;i++) routed[i]=m->L[i].sparse;
        lumi_init_ex(c->n_layers, c->n_experts, c->latent, routed);
        free(routed);
        atexit(lumi_report);
    }
#endif
"""),
    hook("""        experts_apply_union(m,li,nu,uid,pfirst,pcnt,poslist,wlist,z,LT,u,gate,up,hz);
""", """#ifdef LUMIBRI_P2P
        if(lumi_layer_on(li)){
            /* the union the engine just built, executed on peers; the router
             * weights are applied at accumulation there exactly as
             * expert_apply does here */
            lumi_moe_apply_union(li,nu,uid,pfirst,pcnt,poslist,wlist,z,LT,u);
            nu=0;
        }
#endif
""", where="before"),
]

ENGINES = {
    "olmoe.c": OLMOE,
    "colibri.c": COLIBRI,
    "inkling.c": INKLING,
    "kimi_k3.c": KIMI,
}


def apply_hooks(src, hooks, name):
    out = src
    for h in hooks:
        n = out.count(h["anchor"])
        if n != h["count"]:
            raise SystemExit(
                "%s: anchor found %d times, expected %d — upstream moved:\n%s"
                % (name, n, h["count"], h["anchor"][:120]))
        if h["where"] == "after":
            out = out.replace(h["anchor"], h["anchor"] + h["add"], 1)
        else:
            out = out.replace(h["anchor"], h["add"] + h["anchor"], 1)
    return out


def pristine(engine_dir, fname):
    """The engine as upstream ships it.

    A working copy that already has the patch applied would otherwise be
    read as the baseline, and the generator would happily emit a patch that
    installs every hook a second time. So when the file on disk already
    mentions the macro, go back to the last commit for it.
    """
    path = os.path.join(engine_dir, fname)
    if not os.path.exists(path):
        return None
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    if "LUMIBRI_P2P" not in src:
        return src
    rel = subprocess.run(["git", "-C", engine_dir, "ls-files", "--full-name", fname],
                         capture_output=True, text=True)
    top = subprocess.run(["git", "-C", engine_dir, "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True)
    if rel.returncode or not rel.stdout.strip():
        raise SystemExit("%s already has the patch applied and is not in git — "
                         "point --engine-dir at a clean checkout" % fname)
    blob = subprocess.run(["git", "-C", top.stdout.strip(), "show",
                           "HEAD:" + rel.stdout.strip()],
                          capture_output=True, text=True, errors="surrogateescape")
    if blob.returncode:
        raise SystemExit("%s: cannot read the pristine version from git" % fname)
    print("  (%s is already patched on disk — using HEAD as the baseline)" % fname)
    return blob.stdout


def make(engine_dir, outdir):
    written = []
    for fname, hooks in sorted(ENGINES.items()):
        src = pristine(engine_dir, fname)
        if src is None:
            print("  skip %s (not in %s)" % (fname, engine_dir))
            continue
        patched = apply_hooks(src, hooks, fname)
        rel = "c/" + fname
        diff = difflib.unified_diff(
            src.splitlines(keepends=True), patched.splitlines(keepends=True),
            fromfile="a/" + rel, tofile="b/" + rel, n=3)
        body = "".join(diff)
        stem = fname[:-2]
        dst = os.path.join(outdir, "%s-p2p.diff" % stem)
        with open(dst, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write("diff --git a/%s b/%s\n" % (rel, rel))
            f.write(body)
        written.append(dst)
        print("  %s → %s" % (fname, os.path.basename(dst)))
    return written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine-dir", default=os.environ.get("ENGINE", "../moe-stream/c"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        tmp = tempfile.mkdtemp()
        try:
            make(a.engine_dir, tmp)
            bad = 0
            for f in sorted(os.listdir(tmp)):
                new, old = os.path.join(tmp, f), os.path.join(HERE, f)
                if not os.path.exists(old) or open(new).read() != open(old).read():
                    print("STALE: %s differs from the generated one" % f)
                    bad = 1
            sys.exit(bad)
        finally:
            shutil.rmtree(tmp)
    make(a.engine_dir, HERE)


if __name__ == "__main__":
    main()
