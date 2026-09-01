#!/usr/bin/env bash
# The --fallback flag must MEAN something: an origin that streams from disk
# is chosen only when no resident executor covers its layers. Here the
# origins are direct (probed, low latency) and marked --fallback; the donor
# covers 0:2 relay-only (never probed, pessimistic prior). The chain must
# still put the donor on 0:2 — compute rank beats network rank.
set -euo pipefail
cd "$(dirname "$0")"
: "${OLMOE_EDGE_MODEL:?set OLMOE_EDGE_MODEL}"
SEGMENT_NODE_BIN=${SEGMENT_NODE_BIN:-./segment_node}
SEGMENT_CHAT_BIN=${SEGMENT_CHAT_BIN:-./segment_chat}
TRACKER_BIN=${TRACKER_BIN:-./tracker}
T=$(mktemp -d /tmp/lumabri-seg-priority.XXXXXX)
PIDS=()
cleanup() {
    local status=$?
    if [ "$status" -ne 0 ]; then
        for log in "$T"/*.log; do
            [ -f "$log" ] && { echo "--- $log" >&2; tail -40 "$log" >&2; }
        done
    fi
    kill "${PIDS[@]}" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT
export LUMABRI_PEER_BINDINGS="$T/bindings"
root=$(printf '%064x' 4); tok=$(printf '%064x' 20)
E=(env OMP_NUM_THREADS=2 LUMABRI_PEER_KEY=$T/n.k LUMABRI_KNOWN_HOSTS=$T/n.h)

env LUMABRI_PEER_KEY=$T/t.k LUMABRI_KNOWN_HOSTS=$T/t.h \
    "$TRACKER_BIN" --port 8086 --peer-bindings $T/b >$T/tracker.log 2>&1 &
PIDS+=($!)
sleep 1
start_node() { # name range port fallback... extra
    local name=$1 range=$2 port=$3; shift 3
    "${E[@]}" "$SEGMENT_NODE_BIN" --engine olmoe --model-dir "$OLMOE_EDGE_MODEL" \
      --model tiny --range $range --port $port --tracker 127.0.0.1:8086 \
      --name $name --model-root $root --tokenizer-root $tok \
      --context 64 --max-rows 16 --sessions 4 "$@" >$T/$name.log 2>&1 &
    PIDS+=($!)
}
# origins: direct, probed, marked fallback — they cover everything
start_node origin-left  0:2 8087 --advertise 127.0.0.1:8087 --fallback
start_node origin-right 2:4 8088 --advertise 127.0.0.1:8088 --fallback
# the resident donor: covers 0:2, RELAY-ONLY (advertised address refuses
# instantly), never probed — the pessimistic-prior case from the field
start_node donor-left   0:2 8089 --advertise 255.255.255.255:8089
sleep 5

"${E[@]}" LUMABRI_SEGMENT_DEBUG_ROUTES=1 "$SEGMENT_CHAT_BIN" --engine olmoe \
  --model-dir "$OLMOE_EDGE_MODEL" --model tiny --tracker 127.0.0.1:8086 \
  --model-root $root --tokenizer-root $tok \
  --prompt-ids 3,11,29,7,41,19 --tokens 2 \
  --context 64 --max-rows 16 >$T/chat.out 2>&1 || {
    cat $T/chat.out; echo "chat failed"; exit 1; }
chain=$(grep -a "route generation" $T/chat.out | head -1)
echo "$chain" | grep -q "donor-left\[0:2\]" || {
    echo "the chain ignored the resident donor and rode the fallback origin:"
    echo "$chain"; exit 1
}
echo "$chain" | grep -q "origin-left" && {
    echo "origin-left is in the chain although donor-left is resident:"
    echo "$chain"; exit 1
}
echo "SEGMENT PRIORITY: PASS (resident relay donor outranks direct fallback origin)"
