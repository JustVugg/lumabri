#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

make -s tracker expert_node test_relay_exec fixture
T=$(mktemp -d /tmp/lumabri-relay-exec.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

./tracker --port 7560 >"$T/tracker.log" 2>&1 & PIDS+=($!)
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 ./expert_node \
    --model tiny_olmoe --port 7561 --name nat-expert \
    --tracker 127.0.0.1:7560 --advertise 127.0.0.1:1 \
    >"$T/node.log" 2>&1 & PIDS+=($!)

for _ in $(seq 1 200); do
    grep -q "expert nat-expert" "$T/tracker.log" && break
    sleep .1
done
grep -q "expert nat-expert" "$T/tracker.log" || {
    cat "$T/tracker.log" "$T/node.log"; exit 1;
}

./test_relay_exec 127.0.0.1:7560 127.0.0.1:7561 tiny_olmoe 8 1024 0
