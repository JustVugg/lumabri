#!/usr/bin/env bash
# lumabri phase 2 — the experiment.
#
# The same model, the same prompt, generated twice:
#   A) LOCAL   — the engine reads expert weights and runs them itself
#   B) P2P     — the engine keeps only dense+router+KV; every routed expert
#                runs on a peer process that holds it, reached over TCP
#
# The tokens must be IDENTICAL. If they are not, the network has changed the
# model and the design is wrong — that is the only result that matters here.
# The tok/s of both runs is reported alongside, with the honest caveat that
# peers and chatter share this one machine's cores.
set -euo pipefail
cd "$(dirname "$0")"

MODEL="${MODEL:-$PWD/tiny_olmoe}"
NODES="${NODES:-4}"          # how many peers to split the experts across
PORT0="${PORT0:-7401}"
CACHE="${CACHE:-16}"         # local-run expert cache slots per layer

make -s phase2
[ -f "$MODEL/config.json" ] || make -s fixture

PIDS=()
T=$(mktemp -d /tmp/lumabri-phase2.XXXXXX)
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

echo
echo "══ A) LOCAL — experts read and run by the engine itself"
SNAP="$MODEL" OMP_NUM_THREADS=6 ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" \
    > local.out 2>local.err || { cat local.err; exit 1; }
grep -E "^C engine|^Speed" local.out

echo
echo "══ starting $NODES expert peers (each holds 1/$NODES of the experts)"
ADDRS=""
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    # A peer must not let OpenMP default to every logical core: with several
    # peers and the chatter on one host that oversubscribes badly — measured
    # 7.4 ms per expert instead of 0.6 ms. On real separate machines a peer
    # would take the whole box.
    OMP_NUM_THREADS="${NODE_THREADS:-2}" COLI_NO_OMP_TUNE=1 \
    ./expert_node --model "$MODEL" --port "$p" --name "node-$i" \
                  --stride "$NODES:$i" & PIDS+=($!)
    ADDRS="${ADDRS:+$ADDRS,}127.0.0.1:$p"
done
# wait for every peer to finish loading and accept connections
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    for _ in $(seq 1 200); do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; break; }
        sleep 0.2
    done
done

echo
echo "══ B) P2P — every routed expert runs on a peer"
SNAP="$MODEL" OMP_NUM_THREADS=6 LUMABRI_EXPERTS="$ADDRS" \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" > p2p.out 2>p2p.err || { cat p2p.err; exit 1; }
grep -E "^C engine|^Speed" p2p.out
grep -E "^\[lumabri\]" p2p.err || true

echo
A=$(grep "^C engine" local.out)
B=$(grep "^C engine" p2p.out)
if [ "$A" = "$B" ]; then
    echo "✓ IDENTICI — la rete non ha cambiato un solo token"
    echo "  $A"
else
    echo "✗ DIVERGENZA — il P2P ha cambiato l'output:"
    echo "  local: $A"
    echo "  p2p  : $B"
    exit 1
fi

echo
echo "══ C) COMPATIBILITY — a different engine build must be refused"
cat > "$T/wrong_build.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    int lfd = lmb_listen(atoi(argv[1]));
    if (lfd < 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        LmbMsg m;
        while (lmb_recv(fd, &m) == 0) {
            if (m.op == LMB_PING) lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            else if (m.op == LMB_EMANIFEST) {
                LmbBuf b = {0};
                lmb_buf_u32(&b, LMB_EXPERT_MANIFEST_MAGIC);
                lmb_buf_str(&b, "olmoe");
                lmb_buf_str(&b, "source=definitely-not-this-build");
                lmb_buf_str(&b, "tiny_olmoe");
                lmb_buf_u32(&b, 0);       /* effective quant bits */
                lmb_buf_u32(&b, 0);       /* no model identity */
                lmb_buf_u32(&b, 0);       /* no expert claims */
                lmb_buf_u32(&b, 0);       /* deliberately wrong hidden size */
                lmb_send(fd, LMB_EMANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
                free(b.p);
            } else lmb_send(fd, LMB_ERR, NULL, 0, NULL, 0);
            lmb_msg_free(&m);
        }
        close(fd);
    }
}
EOF
cc -O2 -w -I. "$T/wrong_build.c" -o "$T/wrong_build" -lpthread
BAD_PORT=$((PORT0+NODES+50))
"$T/wrong_build" "$BAD_PORT" & PIDS+=($!)
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/$BAD_PORT) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.05
done
set +e
SNAP="$MODEL" OMP_NUM_THREADS=2 LUMABRI_EXPERTS="127.0.0.1:$BAD_PORT" \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" \
    > "$T/wrong.out" 2> "$T/wrong.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "✗ incompatible expert node was accepted"; exit 1; }
grep -q "incompatible manifest" "$T/wrong.err" || {
    echo "✗ refusal did not identify the manifest mismatch"
    cat "$T/wrong.err"
    exit 1
}
echo "✓ incompatible model/build/engine profile refused before execution"
