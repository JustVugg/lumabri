#!/usr/bin/env bash
# lumabri phase 2 on the GLM engine — the same experiment as phase2_test.sh,
# against colibri.c instead of olmoe.c.
#
# It exists because GLM is not olmoe with different numbers:
#
#   · the first layers are DENSE and route nothing, and there is an MTP row
#     at index n_layers that does — so "every layer has n_experts experts" is
#     false in both directions;
#   · the engine computes an expert over ALL the rows a layer routed to it in
#     one call. Sending those rows one at a time gives different floats — not
#     wrong, just different accumulation order — and the generation drifts
#     visibly by the fifth token. Measured, then fixed by batching.
#
# The tokens must be IDENTICAL. That is the only result that matters.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../moe-stream/c}"
MODEL="${MODEL:-$ENGINE/glm_tiny_i4}"
REF="${REF:-$ENGINE/ref_glm.json}"
NODES="${NODES:-2}"
PORT0="${PORT0:-7441}"
BITS="${BITS:-8}"
CAP="${CAP:-8}"

[ -d "$MODEL" ] || { echo "no GLM fixture at $MODEL — set MODEL=<dir>"; exit 1; }
[ -f "$REF" ]   || { echo "no oracle at $REF — set REF=<file>"; exit 1; }

make -s phase2-glm colibri_p2p ENGINE="$ENGINE"

T=$(mktemp -d /tmp/lumabri-glm.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

run() { SNAP="$MODEL" REF="$REF" COLI_NO_OMP_TUNE=1 OMP_NUM_THREADS="${THREADS:-4}" \
        "$@" ./colibri_p2p "$CAP"; }

echo
echo "══ THREAD POLICY — default GLM node uses physical cores, not SMT threads"
POLICY_PORT=$((PORT0+60))
env -u OMP_NUM_THREADS -u OMP_PROC_BIND -u COLI_NO_OMP_TUNE -u COLI_OMP_TUNED \
    ./expert_node_glm --model "$MODEL" --port "$POLICY_PORT" --name glm-policy \
                      --bits "$BITS" --cache 1 > "$T/policy.log" 2>&1 & POLICY_PID=$!; PIDS+=($!)
for _ in $(seq 1 300); do
    (exec 3<>/dev/tcp/127.0.0.1/$POLICY_PORT) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.1
done
grep -q "applying the engine hot-thread policy" "$T/policy.log" || {
    echo "GLM node did not apply its OpenMP hot-thread policy"; cat "$T/policy.log"; exit 1; }
POLICY_LINE=$(grep "thread.* each, .* physical core" "$T/policy.log" | tail -1)
PT=$(printf '%s\n' "$POLICY_LINE" | sed -nE 's/.* ([0-9]+) threads? each, ([0-9]+) physical cores?.*/\1/p')
PC=$(printf '%s\n' "$POLICY_LINE" | sed -nE 's/.* ([0-9]+) threads? each, ([0-9]+) physical cores?.*/\2/p')
[ -n "$PT" ] && [ -n "$PC" ] && [ "$PT" -le "$PC" ] || {
    echo "GLM node oversubscribed its physical cores: $POLICY_LINE"; exit 1; }
kill "$POLICY_PID" 2>/dev/null || true
wait "$POLICY_PID" 2>/dev/null || true
echo "✓ GLM default: $PT OpenMP threads on $PC available physical cores"

echo
echo "══ A) LOCAL — experts read and run by the engine itself"
run env > "$T/local.out" 2>"$T/local.err" || { cat "$T/local.err"; exit 1; }
grep -E "^GLM C engine" "$T/local.out"

echo
echo "══ starting $NODES expert peers (each holds 1/$NODES of the experts)"
ADDRS=""
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    OMP_NUM_THREADS="${NODE_THREADS:-2}" COLI_NO_OMP_TUNE=1 \
    ./expert_node_glm --model "$MODEL" --port "$p" --name "gnode-$i" \
                      --bits "$BITS" --stride "$NODES:$i" > "$T/node$i.log" 2>&1 & PIDS+=($!)
    ADDRS="${ADDRS:+$ADDRS,}127.0.0.1:$p"
done
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    for _ in $(seq 1 300); do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; break; }
        sleep 0.2
    done
done
grep -h "layer slots are dense\|holding" "$T/node0.log" || true

echo
echo "══ B) P2P — every routed expert runs on a peer"
run env LUMABRI_EXPERTS="$ADDRS" > "$T/p2p.out" 2>"$T/p2p.err" || { cat "$T/p2p.err"; exit 1; }
grep -E "^GLM C engine" "$T/p2p.out"
grep -E "^\[lumabri\]" "$T/p2p.err" || true

echo
A=$(grep "^GLM C engine" "$T/local.out")
B=$(grep "^GLM C engine" "$T/p2p.out")
if [ "$A" = "$B" ]; then
    echo "✓ IDENTICI — la rete non ha cambiato un solo token"
    echo "  $A"
else
    echo "✗ DIVERGENZA — il P2P ha cambiato l'output:"
    echo "  local: $A"
    echo "  p2p  : $B"
    exit 1
fi
