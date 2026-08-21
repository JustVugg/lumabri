#!/usr/bin/env bash
# lumabri phase 2 on the qwen36 (Qwen3.6) engine — the experiment.
#
# The same model, the same prompt, generated twice:
#   A) LOCAL   — the engine reads expert weights and runs them itself
#   B) P2P     — the chatter keeps dense+router+KV; every routed expert runs on
#                a peer process that holds it, reached over TCP
# The generated tokens must be IDENTICAL. That is the only result that matters.
#
# No synthetic fixture ships here: a qwen36 container is built by colibri's own
# tools (they need torch + transformers), so point MODEL at one. Build a tiny
# one with, from a colibri checkout:
#
#   python tools/make_qwen36_tiny.py --out /tmp/qw_hf --emit-ref /tmp/qw_hf/ref.json
#   python tools/convert_qwen36.py  --model /tmp/qw_hf --out /tmp/qw --ebits 4   # add --gs 64 for the group-scaled path
#   cp /tmp/qw_hf/ref.json /tmp/qw/ref.json
#   MODEL=/tmp/qw ./phase2_qwen36_test.sh
#
# qwen36 computes an expert one row at a time (olmoe's dialect), so the per-row
# EXEC matches the local path exactly. Its two specifics — group-scaled experts
# (matmul_qe/expert_gs) and int4/int8 in one container (slot_ensure_int8) — are
# what the donor glue has to reproduce; --gs 64 is the container that exercises
# the first, and this test is what proves it.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../colibri/c}"
MODEL="${MODEL:-$PWD/qwen36_tiny}"
NODES="${NODES:-4}"          # how many peers to split the experts across
PORT0="${PORT0:-7571}"
CACHE="${CACHE:-16}"         # local-run expert cache slots per layer
GEN="${GEN:-16}"
BITS="${BITS:-4}"

[ -f "$MODEL/config.json" ] && [ -f "$MODEL/qwen36_meta.json" ] || {
    echo "no qwen36 container at $MODEL (need config.json + qwen36_meta.json)"
    echo "build one with colibri's make_qwen36_tiny.py + convert_qwen36.py — see the header"
    exit 1; }
REF="$MODEL/ref.json"; [ -f "$REF" ] || REF="ref.json"

make -s expert_node_qwen36 qwen36_p2p ENGINE="$ENGINE"

T=$(mktemp -d /tmp/lumabri-qwen36.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

echo
echo "══ A) LOCAL — experts read and run by the engine itself"
SNAP="$MODEL" OMP_NUM_THREADS="${THREADS:-6}" \
    ./qwen36_p2p "$CACHE" "$BITS" "$REF" > "$T/local.out" 2>"$T/local.err" || true
A=$(grep "^C engine" "$T/local.out" || true)
[ -n "$A" ] || { echo "no tokens from the local run"; tail -20 "$T/local.err"; exit 1; }
echo "  $A"

echo
echo "══ starting $NODES expert peers (each holds 1/$NODES of the experts)"
ADDRS=""
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    OMP_NUM_THREADS="${NODE_THREADS:-2}" COLI_NO_OMP_TUNE=1 \
    ./expert_node_qwen36 --model "$MODEL" --port "$p" --name "qw-$i" \
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
grep -h "holding" "$T/node0.log" | head -1 || { tail -10 "$T/node0.log"; exit 1; }

echo
echo "══ B) P2P — every routed expert runs on a peer"
SNAP="$MODEL" OMP_NUM_THREADS="${THREADS:-6}" LUMABRI_EXPERTS="$ADDRS" \
    ./qwen36_p2p "$CACHE" "$BITS" "$REF" > "$T/p2p.out" 2>"$T/p2p.err" || true
B=$(grep "^C engine" "$T/p2p.out" || true)
echo "  $B"
grep -E "^\[lumabri\]" "$T/p2p.err" | grep -iE "phase 2 active|remote expert|incompatible" | head

echo
grep -q "phase 2 active" "$T/p2p.err" || { echo "✗ phase 2 never engaged"; exit 1; }
if [ -n "$A" ] && [ "$A" = "$B" ]; then
    echo "✓ IDENTICI — la rete non ha cambiato un solo token"
else
    echo "✗ DIVERGENZA — il P2P ha cambiato l'output:"
    echo "  local: $A"
    echo "  p2p  : $B"
    exit 1
fi
