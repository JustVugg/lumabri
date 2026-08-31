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
# per-attempt failover window for the wedged-holder stage (read by the tracker)
export LUMABRI_RELAY_TRY_S=3

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

echo "· concurrency: four relayed calls to one delayed node finish together"
# a fresh tracker: the delayed node is its only holder, no staleness dance
./tracker --port 7565 >"$T/tracker2.log" 2>&1 & PIDS+=($!)
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 LUMABRI_EXEC_DELAY_MS=1000 ./expert_node \
    --model tiny_olmoe --port 7562 --name slow-expert \
    --tracker 127.0.0.1:7565 --advertise 127.0.0.1:2 \
    >"$T/slow.log" 2>&1 & PIDS+=($!)
SLOW_PID=$!
for _ in $(seq 1 600); do
    grep -q "expert slow-expert" "$T/tracker2.log" && break
    sleep .1
done
t0=$(date +%s)
declare -A CONC
for i in 1 2 3 4; do
    ./test_relay_exec 127.0.0.1:7565 127.0.0.1:7562 tiny_olmoe 8 1024 0 \
        >"$T/conc$i.log" 2>&1 & CONC[$i]=$!
done
conc_bad=0
for i in 1 2 3 4; do wait "${CONC[$i]}" || conc_bad=1; done
elapsed=$(( $(date +%s) - t0 ))
if [ "$conc_bad" -ne 0 ]; then
    echo "   a concurrent relayed call failed"; cat "$T"/conc*.log; exit 1
fi
# each binary performs 2 delayed calls (direct + relayed): serial would be
# 4 clients x 2 calls x 1 s = 8 s; parallel workers finish in ~2-3 s
if [ "$elapsed" -ge 7 ]; then
    echo "   four concurrent relayed calls took ${elapsed}s — serialized"; exit 1
fi
echo "   ✓ four clients through one delayed node in ${elapsed}s"

echo "· failover: a wedged holder must not eat the call when a live one exists"
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 ./expert_node \
    --model tiny_olmoe --port 7563 --name second-expert \
    --tracker 127.0.0.1:7565 --advertise 127.0.0.1:3 \
    >"$T/second.log" 2>&1 & PIDS+=($!)
for _ in $(seq 1 600); do
    grep -q "expert second-expert" "$T/tracker2.log" && break
    sleep .1
done
kill -STOP "$SLOW_PID"   # wedge, not kill: registration stays fresh briefly
t0=$(date +%s)
./test_relay_exec 127.0.0.1:7565 127.0.0.1:7563 \
    tiny_olmoe 8 1024 0 >"$T/failover.log" 2>&1 || {
    echo "   the relayed call failed although a live holder existed"
    kill -CONT "$SLOW_PID" 2>/dev/null || true
    cat "$T/failover.log"; exit 1
}
elapsed=$(( $(date +%s) - t0 ))
kill -CONT "$SLOW_PID" 2>/dev/null || true
echo "   ✓ answered by the live replica in ${elapsed}s"

echo "LUMABRI RELAY EXEC TEST: PASS (direct, tunnel, concurrency, failover)"
