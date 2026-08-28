#!/usr/bin/env bash
# User-facing gate: launch the ordinary serve/chat commands, intentionally ask
# for more context than the tiny origin advertises, and require Segment rather
# than allowing the classic fallback to hide an integration regression.
set -euo pipefail
cd "$(dirname "$0")"

: "${OLMOE_EDGE_MODEL:?set OLMOE_EDGE_MODEL}"

TMP=$(mktemp -d /tmp/lumabri-segment-native.XXXXXX)
SERVER_PID=""
cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

wait_port() {
    local port=$1
    for _ in $(seq 1 400); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3<&-; exec 3>&-; return 0
        fi
        sleep 0.025
    done
    return 1
}

mkdir -p "$TMP/server-home" "$TMP/client-home"
HOME="$TMP/server-home" ./lumabri key --out "$TMP/swarm" >/dev/null

HOME="$TMP/server-home" LUMABRI_SEGMENT_CHUNKS=2 OMP_NUM_THREADS=2 \
    stdbuf -oL -eL ./lumabri serve --model "$OLMOE_EDGE_MODEL" --port 7880 \
    --key "$TMP/swarm.key" \
    >"$TMP/server.log" 2>&1 &
SERVER_PID=$!

if ! wait_port 7880 || ! wait_port 7883 || ! wait_port 7884; then
    cat "$TMP/server.log"
    echo "SEGMENT NATIVE: origin did not become ready" >&2
    exit 1
fi

set +e
printf 'hi\n/quit\n' | HOME="$TMP/client-home" OMP_NUM_THREADS=2 \
    LUMABRI_PUBKEY="$TMP/swarm.pub" LUMABRI_SEGMENT_REQUIRED=1 \
    ./lumabri chat --plain --tracker 127.0.0.1:7880 --ctx 2048 \
    --max-new 2 --role chat >"$TMP/chat.log" 2>&1
status=$?
set -e
if (( status != 0 )) || ! grep -q 'pronto via Segment' "$TMP/chat.log" ||
   ! grep -q 'Segment context negotiated to' "$TMP/chat.log" ||
   ! grep -q 'data plane relay (nessuna porta pubblica richiesta)' "$TMP/server.log" ||
   grep -q 'continuo con il percorso expert/CAS' "$TMP/chat.log" ||
   grep -q 'Segment route generation' "$TMP/chat.log"; then
    cat "$TMP/server.log" "$TMP/chat.log"
    echo "SEGMENT NATIVE: ordinary serve/chat gate failed" >&2
    exit 1
fi

echo "SEGMENT NATIVE: PASS (serve + TUI chat, NAT relay + context negotiation)"
