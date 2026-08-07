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

make -s phase2-glm ENGINE="$ENGINE"

T=$(mktemp -d /tmp/lumabri-glm.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

# The patched engine is built from a COPY: colibri itself is never modified.
cp "$ENGINE/colibri.c" "$T/colibri.c"
( cd "$T" && patch -s -p2 < "$OLDPWD/engine_patches/colibri-p2p.diff" )
cc -O2 -fopenmp -Wall -I. -I"$ENGINE" -Wno-unused-function -Wno-unused-parameter \
   -DLUMABRI_P2P -DLUMIBRI_P2P "$T/colibri.c" -o "$T/colibri_p2p" -lm -lpthread

run() { SNAP="$MODEL" REF="$REF" COLI_NO_OMP_TUNE=1 OMP_NUM_THREADS="${THREADS:-4}" \
        "$@" "$T/colibri_p2p" "$CAP"; }

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
