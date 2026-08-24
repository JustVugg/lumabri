#!/usr/bin/env bash
# Partial phase 2: HALF the experts live on one peer, the other half on
# nobody. The old policy kept phase 2 dark for one uncovered expert anywhere
# — donors idle at 99% coverage. Now coverage gates per LAYER: covered
# layers run on the swarm, the rest run locally from the snapshot, and the
# generated tokens must be IDENTICAL to an all-local run. That identity is
# the only result that matters.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../colibri/c}"
MODEL="${MODEL:-$PWD/tiny_olmoe}"
PORT="${PORT:-7651}"
CACHE=16
GEN="${GEN:-16}"

make -s tracker expert_node olmoe_p2p ENGINE="$ENGINE"
[ -f "$MODEL/config.json" ] || make -s fixture

T=$(mktemp -d /tmp/lumabri-partial.XXXXXX)
export LUMABRI_PEER_BINDINGS="$T/peer-bindings"
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 300); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    return 1
}

TOTAL=$(python3 -c "
import json; c=json.load(open('$MODEL/config.json'))
print(c['num_hidden_layers']*c['num_experts'])")
HALF=$((TOTAL / 2))

echo "══ A) all local — the truth"
SNAP="$MODEL" OMP_NUM_THREADS=2 \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" > "$T/local.out" 2>"$T/local.err" || true
A=$(grep "^C engine" "$T/local.out" || true)
[ -n "$A" ] || { echo "no tokens from the local run"; tail -20 "$T/local.err"; exit 1; }
echo "  $A"

echo
echo "══ one peer holding HALF the experts ($HALF of $TOTAL, whole layers)"
./tracker --port "$PORT" > "$T/tracker.log" 2>&1 & PIDS+=($!)
wait_port "$PORT"
OMP_NUM_THREADS=2 ./expert_node --model "$MODEL" --port $((PORT+1)) \
    --tracker "127.0.0.1:$PORT" --advertise "127.0.0.1:$((PORT+1))" \
    --model-name tiny_olmoe --name partial-0 --hold "$HALF" \
    > "$T/node.log" 2>&1 & PIDS+=($!)
wait_port $((PORT+1))
for _ in $(seq 1 100); do
    grep -q "+ expert partial-0" "$T/tracker.log" && break
    sleep 0.2
done
grep -q "+ expert partial-0" "$T/tracker.log" || { echo "node never registered"; exit 1; }

echo
echo "══ B) partial swarm — covered layers remote, the rest local"
SNAP="$MODEL" OMP_NUM_THREADS=2 \
    LUMABRI_TRACKER="127.0.0.1:$PORT" LUMABRI_MODEL=tiny_olmoe \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" > "$T/p2p.out" 2>"$T/p2p.err" || true
B=$(grep "^C engine" "$T/p2p.out" || true)
echo "  $B"
grep -E "phase 2 partial|remote expert" "$T/p2p.err" | head -3

grep -q "phase 2 partial" "$T/p2p.err" || {
    echo "✗ partial phase 2 never engaged"; cat "$T/p2p.err" | head; exit 1; }
grep -qE "[1-9][0-9]* remote expert calls" "$T/p2p.err" || {
    echo "✗ no expert ever ran remotely"; exit 1; }
if [ "$A" = "$B" ]; then
    echo "✓ IDENTICI — metà sciame, metà locale, non un token cambiato"
else
    echo "✗ DIVERGENZA:"; echo "  local  : $A"; echo "  partial: $B"; exit 1
fi
