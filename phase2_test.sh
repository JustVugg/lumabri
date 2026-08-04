#!/usr/bin/env bash
# lumibri phase 2 — the experiment.
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
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; }
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
SNAP="$MODEL" OMP_NUM_THREADS=6 LUMIBRI_EXPERTS="$ADDRS" \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" > p2p.out 2>p2p.err || { cat p2p.err; exit 1; }
grep -E "^C engine|^Speed" p2p.out
grep -E "^\[lumibri\]" p2p.err || true

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
