#!/usr/bin/env bash
# Real tracker gate for Segment discovery. The C test keeps every signed SREG
# control connection open while it checks leases, replicas, draining and the
# asynchronous route snapshot used by inference.
set -euo pipefail
cd "$(dirname "$0")"

T=$(mktemp -d /tmp/lumabri-segdisc.XXXXXX)
TRACKER_PID=""
cleanup() {
    if [[ -n "$TRACKER_PID" ]]; then kill "$TRACKER_PID" 2>/dev/null || true; fi
    rm -rf "$T"
}
trap cleanup EXIT

export HOME="$T"
./tracker --port 7668 --peer-bindings "$T/bindings" >"$T/tracker.log" 2>&1 &
TRACKER_PID=$!
wait_tracker() {
    for _ in $(seq 1 100); do
        if (exec 3<>/dev/tcp/127.0.0.1/7668) 2>/dev/null; then
            exec 3<&-; exec 3>&-; return 0
        fi
        sleep 0.05
    done
    return 1
}
wait_tracker
if ! kill -0 "$TRACKER_PID" 2>/dev/null; then
    cat "$T/tracker.log"
    exit 1
fi

if ! ./test_segment_discovery 127.0.0.1:7668; then
    cat "$T/tracker.log"
    exit 1
fi

# A tracker restart must reserve a strictly newer high generation epoch, so a
# chatter cannot confuse a fresh route with a cached pre-restart placement.
FIRST_EPOCH=$(tr -d '\n' <"$T/bindings.segment-generation")
kill "$TRACKER_PID"
wait "$TRACKER_PID" 2>/dev/null || true
TRACKER_PID=""
./tracker --port 7668 --peer-bindings "$T/bindings" >"$T/tracker-restart.log" 2>&1 &
TRACKER_PID=$!
wait_tracker
SECOND_EPOCH=$(tr -d '\n' <"$T/bindings.segment-generation")
if (( SECOND_EPOCH <= FIRST_EPOCH )); then
    echo "Segment route generation did not advance across tracker restart"
    cat "$T/tracker-restart.log"
    exit 1
fi
echo "SEGMENT GENERATION: PASS (durable across tracker restart)"
