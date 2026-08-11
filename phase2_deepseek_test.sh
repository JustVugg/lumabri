#!/usr/bin/env bash
# lumabri phase 2 on the DeepSeek V4 engine.
#
# The one engine with no synthetic fixture: V4's loader validates a strict
# config and an FP8/FP4 tensor plan, so a random-weight stand-in is more work
# than it is worth. It runs against a real model instead — point MODEL at one.
#
# What makes V4 different, and why this test is the one that matters most:
#
#   · coli_v4_expert_forward_ref folds the router weight in BEFORE the down
#     projection and rounds the product to bf16. `w · expert(x)` is therefore
#     NOT what the engine computes, so the weight travels with the activation
#     and the peer applies it. A chatter-side multiply would have looked
#     right and been wrong.
#   · three separate places apply a target expert — one per-token, one in the
#     block pipeline, one in the batch union — and each had to be hooked.
#     Missing one is not a crash, it is a layer that quietly ran locally.
#
# The engine scores itself: the local run writes an oracle, the P2P run is
# validated against it by --oracle. 10/10 and 4/4 is the engine agreeing.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../moe-stream/c}"
MODEL="${MODEL:-$HOME/deepseek_v4}"
PORT="${PORT:-7473}"
GEN="${GEN:-4}"
PROMPT="${PROMPT:-Ciao}"
CHAT_GB="${CHAT_GB:-10}"       # the chatter still holds the dense side
NODE_CACHE="${NODE_CACHE:-6}"  # expert slots per layer on the peer

[ -f "$MODEL/config.json" ] || {
    echo "no DeepSeek V4 model at $MODEL"
    echo "set MODEL=<dir> (tools/download_deepseek_v4.py fetches one)"
    exit 1; }

make -s expert_node_deepseek deepseek_p2p ENGINE="$ENGINE"

T=$(mktemp -d /tmp/lumabri-ds.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

echo
echo "══ A) LOCAL — experts read and run by the engine itself"
OMP_NUM_THREADS="${THREADS:-6}" ./deepseek_p2p "$MODEL" "$PROMPT" \
    --max-tokens "$GEN" --memory-gb "$CHAT_GB" --no-dspark \
    --record-oracle "$T/oracle.json" > "$T/local.log" 2>&1 \
    || { tail -20 "$T/local.log"; exit 1; }
grep -E "^generated_text|^v4_tokens" "$T/local.log" || true

echo
echo "══ starting the expert peer (all $(python3 -c "
import json;c=json.load(open('$MODEL/config.json'))
print(c['num_hidden_layers']*c['n_routed_experts'])") experts, streamed from disk)"
./expert_node_deepseek --model "$MODEL" --port "$PORT" --name ds-0 \
                       --cache "$NODE_CACHE" > "$T/node.log" 2>&1 & PIDS+=($!)
for _ in $(seq 1 600); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.5
done
grep -h "holding" "$T/node.log" || { tail -10 "$T/node.log"; exit 1; }

echo
echo "══ B) P2P — every routed expert runs on the peer, scored against local"
OMP_NUM_THREADS="${THREADS:-6}" LUMABRI_EXPERTS="127.0.0.1:$PORT" \
    ./deepseek_p2p "$MODEL" --oracle "$T/oracle.json" --greedy "$GEN" \
    --memory-gb "$CHAT_GB" --no-dspark > "$T/p2p.log" 2>&1 \
    || { tail -20 "$T/p2p.log"; exit 1; }
grep -E "^\[lumabri\]" "$T/p2p.log" || true
TF=$(grep -E "^PREFILL .*oracle:" "$T/p2p.log" || true)
GD=$(grep -E "^GREEDY .*oracle:" "$T/p2p.log" || true)
echo "  $TF"
echo "  $GD"

echo
grep -q "phase 2 active" "$T/p2p.log" || { echo "✗ phase 2 never engaged"; exit 1; }
OK_TF=$(echo "$TF" | grep -oE "[0-9]+/[0-9]+" | awk -F/ '$1==$2')
OK_GD=$(echo "$GD" | grep -oE "[0-9]+/[0-9]+" | awk -F/ '$1==$2')
if [ -n "$OK_TF" ] && [ -n "$OK_GD" ]; then
    echo "✓ IDENTICI — la rete non ha cambiato un solo token"
    echo "  prefill $OK_TF · generati $OK_GD"
else
    echo "✗ DIVERGENZA — il P2P ha cambiato l'output:"
    echo "  $TF"
    echo "  $GD"
    exit 1
fi
