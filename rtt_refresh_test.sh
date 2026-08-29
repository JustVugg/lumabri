#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
TRACKER_BIN=${TRACKER_BIN:-./tracker}
TEST_BIN=${TEST_BIN:-./test_rtt_refresh}
T=$(mktemp -d /tmp/lumabri-rtt-refresh.XXXXXX)
PIDS=()
cleanup() {
    if ((${#PIDS[@]})); then kill "${PIDS[@]}" 2>/dev/null || true; fi
    if ((${#PIDS[@]})); then wait "${PIDS[@]}" 2>/dev/null || true; fi
    rm -rf "$T"
}
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && {
            exec 3<&-; exec 3>&-; return 0;
        }
        sleep .05
    done
    return 1
}
LUMABRI_PEER_BINDINGS="$T/bindings" "$TRACKER_BIN" --port 8093 \
    >"$T/tracker.log" 2>&1 &
PIDS+=("$!")
wait_port 8093
"$TEST_BIN" 127.0.0.1:8093
echo 'RTT REFRESH TEST: PASS (stale selection corrected, named attribution preserved)'
