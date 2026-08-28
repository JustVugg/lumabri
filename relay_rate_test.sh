#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
TRACKER_BIN=${TRACKER_BIN:-./tracker}
RATE_BIN=${RATE_BIN:-./test_relay_rate}
T=$(mktemp -d /tmp/lumabri-relay-rate.XXXXXX)
PID=
cleanup() {
    [[ -z "$PID" ]] || kill "$PID" 2>/dev/null || true
    [[ -z "$PID" ]] || wait "$PID" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT
LUMABRI_RSEG_RATE=1 LUMABRI_RSEG_BURST=1 \
LUMABRI_PEER_BINDINGS="$T/bindings" "$TRACKER_BIN" --port 8092 >"$T/tracker.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/8092) 2>/dev/null && {
        exec 3<&-; exec 3>&-; break
    }
    sleep .05
done
"$RATE_BIN" 127.0.0.1:8092
echo 'RELAY RATE TEST: PASS (unsigned callers are bounded per source)'
