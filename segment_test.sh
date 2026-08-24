#!/usr/bin/env bash
# Segment execution — the latency protocol. Two peers each hold a CONTIGUOUS
# half of tiny_olmoe's layers (dense, experts, and that half's KV); the
# chatter sends the hidden state and gets it back one segment at a time:
# per token, 16 layer rounds become 2 round trips. The generated tokens must
# be IDENTICAL to an all-local run — first with every layer on a segment,
# then with one segment and the other half local. Identity is the contract.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../colibri/c}"
MODEL="${MODEL:-$PWD/tiny_olmoe}"
PORT="${PORT:-7671}"
CACHE=16

make -s expert_node olmoe_p2p ENGINE="$ENGINE"
[ -f "$MODEL/config.json" ] || make -s fixture

T=$(mktemp -d /tmp/lumabri-segment.XXXXXX)
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

LAYERS=$(python3 -c "
import json; print(json.load(open('$MODEL/config.json'))['num_hidden_layers'])")
HALF=$((LAYERS / 2))

echo "══ A) all local — the truth"
SNAP="$MODEL" OMP_NUM_THREADS=2 \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" > "$T/local.out" 2>"$T/local.err" || true
A=$(grep "^C engine" "$T/local.out" || true)
[ -n "$A" ] || { echo "no tokens from the local run"; tail -20 "$T/local.err"; exit 1; }
echo "  $A"

echo
echo "══ two segment peers: layers 0..$((HALF-1)) and $HALF..$((LAYERS-1))"
OMP_NUM_THREADS=2 CTX=256 ./expert_node --model "$MODEL" --port $((PORT)) \
    --name seg-a --segment "0:$((HALF-1))" > "$T/sega.log" 2>&1 & PIDS+=($!)
OMP_NUM_THREADS=2 CTX=256 ./expert_node --model "$MODEL" --port $((PORT+1)) \
    --name seg-b --segment "$HALF:$((LAYERS-1))" > "$T/segb.log" 2>&1 & PIDS+=($!)
wait_port "$PORT" || { tail "$T/sega.log"; exit 1; }
wait_port $((PORT+1)) || { tail "$T/segb.log"; exit 1; }

echo
echo "══ B) every layer on a segment — 2 round trips per token"
SNAP="$MODEL" OMP_NUM_THREADS=2 \
    LUMABRI_SEGMENTS="127.0.0.1:$PORT=0-$((HALF-1)),127.0.0.1:$((PORT+1))=$HALF-$((LAYERS-1))" \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" > "$T/seg.out" 2>"$T/seg.err" || true
B=$(grep "^C engine" "$T/seg.out" || true)
echo "  $B"
grep -E "segment .*round trip" "$T/seg.err" | head -2
grep -q "segment .* layers 0" "$T/seg.err" || {
    echo "✗ the segments never engaged"; head "$T/seg.err"; exit 1; }
[ "$A" = "$B" ] || { echo "✗ DIVERGENZA (full segments)"; echo "  local: $A"; echo "  seg  : $B"; exit 1; }
echo "  ✓ identici con OGNI layer su un segmento"

echo
echo "══ C) one segment + the other half local — mixed"
SNAP="$MODEL" OMP_NUM_THREADS=2 \
    LUMABRI_SEGMENTS="127.0.0.1:$PORT=0-$((HALF-1))" \
    ./olmoe_p2p "$CACHE" 8 "$MODEL/ref.json" > "$T/mix.out" 2>"$T/mix.err" || true
C=$(grep "^C engine" "$T/mix.out" || true)
echo "  $C"
[ "$A" = "$C" ] || { echo "✗ DIVERGENZA (mixed)"; echo "  local: $A"; echo "  mix  : $C"; exit 1; }
echo "  ✓ identici anche a metà segmento, metà locale"
echo
echo "✓ SEGMENTI — 16 giri di layer diventano 2 round trip, non un token cambiato"
