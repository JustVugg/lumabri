#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

ENGINE_DIR=${ENGINE:-../colibri/c}
if [ ! -f "$ENGINE_DIR/olmoe.c" ]; then
    echo "LUMABRI RELAY EXEC TEST: SKIP (no olmoe.c under ENGINE=$ENGINE_DIR)"
    exit 0
fi
make -s ENGINE="$ENGINE_DIR" tracker expert_node test_relay_exec fixture
T=$(mktemp -d /tmp/lumabri-relay-exec.XXXXXX)
PIDS=()
cleanup() {
    local status=$?
    if [ "$status" -ne 0 ]; then
        for log in "$T"/*.log; do
            [ -f "$log" ] && { echo "--- $log" >&2; tail -80 "$log" >&2; }
        done
    fi
    kill "${PIDS[@]}" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT
export LUMABRI_PEER_BINDINGS="$T/peer-bindings"

./tracker --port 7560 >"$T/tracker.log" 2>&1 & PIDS+=($!)
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 ./expert_node \
    --model tiny_olmoe --port 7561 --name nat-expert \
    --tracker 127.0.0.1:7560 --advertise 127.0.0.1:1 \
    >"$T/node.log" 2>&1 & PIDS+=($!)

for _ in $(seq 1 600); do
    grep -q "expert nat-expert" "$T/tracker.log" && break
    sleep .1
done
grep -q "expert nat-expert" "$T/tracker.log" || {
    cat "$T/tracker.log" "$T/node.log"; exit 1;
}

./test_relay_exec 127.0.0.1:7560 127.0.0.1:7561 tiny_olmoe 8 1024 0
