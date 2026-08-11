#!/usr/bin/env bash
# lumabri phase 2 on the Kimi K3 engine.
#
# K3's routed experts do NOT work in hidden space: the layer projects down to
# routed_expert_hidden_size, routes and runs the experts there, then projects
# back up. That latent is what crosses the wire, and if the peer disagreed
# about it the manifest check would reject it — so this test is also what
# proves the width is right.
#
# With no tokenizer in the fixture the engine prints raw token ids, which is
# exactly the comparison we want. The tokens must be IDENTICAL.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../moe-stream/c}"
NODES="${NODES:-2}"
PORT0="${PORT0:-7485}"   # clear of phase5 (745x) and sign_test (746x)
NGEN="${NGEN:-8}"
IDS="${IDS:-3 7 11 19}"
export K3_BITS=32          # f32 dense side: the question is the experts

make -s expert_node_kimi kimi_k3_p2p ENGINE="$ENGINE"

T=$(mktemp -d /tmp/lumabri-k3.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

MODEL="${MODEL:-$T/tiny_kimi}"
[ -f "$MODEL/config.json" ] || python3 make_tiny_kimi.py "$MODEL" > /dev/null

run() { OMP_NUM_THREADS="${THREADS:-2}" "$@" \
        ./kimi_k3_p2p "$MODEL" --ids "$IDS" --ngen "$NGEN"; }

echo
echo "══ A) LOCAL — experts read and run by the engine itself"
run env > "$T/local.out" 2>"$T/local.err" || { tail -20 "$T/local.err"; exit 1; }
A=$(tr -s ' \n' ' ' < "$T/local.out" | sed 's/^ *//;s/ *$//')
[ -n "$A" ] || { echo "no tokens from the local run"; tail -20 "$T/local.err"; exit 1; }
echo "  tokens: $A"

echo
echo "══ starting $NODES expert peers (each holds 1/$NODES of the experts)"
ADDRS=""
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    OMP_NUM_THREADS="${NODE_THREADS:-2}" \
    ./expert_node_kimi --model "$MODEL" --port "$p" --name "k3-$i" \
                       --stride "$NODES:$i" > "$T/node$i.log" 2>&1 & PIDS+=($!)
    ADDRS="${ADDRS:+$ADDRS,}127.0.0.1:$p"
done
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    for _ in $(seq 1 300); do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; break; }
        sleep 0.2
    done
done
grep -h "dense and route nothing\|holding" "$T/node0.log" || true

echo
echo "══ B) P2P — every routed expert runs on a peer"
run env LUMABRI_EXPERTS="$ADDRS" > "$T/p2p.out" 2>"$T/p2p.err" || { tail -20 "$T/p2p.err"; exit 1; }
B=$(tr -s ' \n' ' ' < "$T/p2p.out" | sed 's/^ *//;s/ *$//')
echo "  tokens: $B"
grep -E "^\[lumabri\]" "$T/p2p.err" || true

echo
if [ "$A" = "$B" ]; then
    echo "✓ IDENTICI — la rete non ha cambiato un solo token"
    echo "  $A"
else
    echo "✗ DIVERGENZA — il P2P ha cambiato l'output:"
    echo "  local: $A"
    echo "  p2p  : $B"
    exit 1
fi
