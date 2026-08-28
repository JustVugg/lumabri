#!/usr/bin/env bash
# Real stateful Segment relay gate. Both executors deliberately suppress their
# direct transport, so OPEN/RUN/SNAPSHOT/CLOSE must traverse the signed tracker
# tunnel. A same-range replica makes the persistent turn take real snapshots;
# greedy output is still compared with the independent OLMoE oracle.
set -euo pipefail
cd "$(dirname "$0")"

: "${OLMOE_EDGE_MODEL:?set OLMOE_EDGE_MODEL}"
: "${OLMOE_EDGE_REF:?set OLMOE_EDGE_REF}"
SEGMENT_NODE_BIN=${SEGMENT_NODE_BIN:-./segment_node}
SEGMENT_CHAT_BIN=${SEGMENT_CHAT_BIN:-./segment_chat}
TRACKER_BIN=${TRACKER_BIN:-./tracker}
TMP=$(mktemp -d /tmp/lumabri-segment-relay.XXXXXX)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$TMP"
}
trap cleanup EXIT

wait_port() {
    local port=$1
    for _ in $(seq 1 200); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3<&-; exec 3>&-; return 0
        fi
        sleep 0.025
    done
    return 1
}

env LUMABRI_PEER_KEY="$TMP/tracker.key" \
    LUMABRI_KNOWN_HOSTS="$TMP/known" \
    "$TRACKER_BIN" --port 7968 --peer-bindings "$TMP/bindings" \
    >"$TMP/tracker.log" 2>&1 &
PIDS+=("$!")
wait_port 7968

model_root=$(printf '%064x' 91)
tokenizer_root=$(printf '%064x' 92)
common=(env OMP_NUM_THREADS=2 LUMABRI_PEER_KEY="$TMP/node.key"
        LUMABRI_KNOWN_HOSTS="$TMP/known")
for spec in "0:2 7969 relay-left" "2:4 7970 relay-right" \
            "0:2 7971 relay-left-replica"; do
    read -r range port name <<<"$spec"
    "${common[@]}" "$SEGMENT_NODE_BIN" --engine olmoe \
        --model-dir "$OLMOE_EDGE_MODEL" --model tiny-relay \
        --range "$range" --port "$port" --tracker 127.0.0.1:7968 \
        --advertise "127.0.0.1:$port" --name "$name" \
        --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
        --context 64 --max-rows 16 --sessions 4 --relay-only --fallback \
        >"$TMP/$name.log" 2>&1 &
    PIDS+=("$!")
    wait_port "$port"
done
sleep 1

oracle=$(python3 -c '
import json,sys
r=json.load(open(sys.argv[1], encoding="utf-8"))
p=r["prompt_ids"]; f=r["full_ids"]
print(",".join(map(str,p))+"|"+",".join(map(str,f[len(p):len(p)+3])))
' "$OLMOE_EDGE_REF")
prompt_ids=${oracle%%|*}
expected=${oracle#*|}
output=$(env OMP_NUM_THREADS=2 LUMABRI_PEER_KEY="$TMP/client.key" \
    LUMABRI_KNOWN_HOSTS="$TMP/known" LUMABRI_SEGMENT_DEBUG_ROUTES=1 \
    "$SEGMENT_CHAT_BIN" --engine olmoe --model-dir "$OLMOE_EDGE_MODEL" \
    --model tiny-relay --tracker 127.0.0.1:7968 \
    --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
    --prompt-ids "$prompt_ids" --tokens 3 --expect-ids "$expected" \
    --context 64 --max-rows 16 2>&1)
grep -q 'relay-left.*transport=2' <<<"$output"
grep -q 'relay-right.*transport=2' <<<"$output"

# Persistent mode forces a real checkpoint through the relay after generation.
serve=$(printf 'SUBMIT 1 0 2 2 0.7 0.95\nhi\n' | \
    env OMP_NUM_THREADS=2 LUMABRI_PEER_KEY="$TMP/client2.key" \
        LUMABRI_KNOWN_HOSTS="$TMP/known" LUMABRI_SAMPLE_SEED=7 \
        "$SEGMENT_CHAT_BIN" --serve --engine olmoe \
        --model-dir "$OLMOE_EDGE_MODEL" --model tiny-relay \
        --tracker 127.0.0.1:7968 --model-root "$model_root" \
        --tokenizer-root "$tokenizer_root" --context 64 --max-rows 16)
grep -q '^DATA 1 ' <<<"$serve"
grep -q '^DONE 1 STAT ' <<<"$serve"
echo "SEGMENT RELAY: PASS (relay-only OPEN/RUN/SNAPSHOT/CLOSE)"
