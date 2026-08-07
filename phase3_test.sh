#!/usr/bin/env bash
# lumabri phase 3 — the war on RTT, verified. Three claims, three proofs:
#
#   1) PROXIMITY  two replicas of the same tree, one emulated 60 ms away:
#                 the shim must take the near one for ~every byte, and stay
#                 byte-identical.
#   2) PREFETCH   a 40 ms swarm, cold mirror: readahead must cut the wall
#                 clock vs the same fetch without it.
#   3) FAILOVER   phase 2, two peers each holding EVERY expert; the busy one
#                 is killed mid-generation; the run must finish with tokens
#                 identical to the local reference, via replica failover.
#
# Needs no root: the distance is emulated inside the serving peer
# (LUMABRI_RTT_US), which the probes see exactly like real flight time.
set -euo pipefail
cd "$(dirname "$0")"

make -s all

T=$(mktemp -d /tmp/lumabri-phase3.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

mkdir -p "$T/src"
head -c $((24 * 1024 * 1024)) /dev/urandom > "$T/src/w.bin"
head -c 777 /dev/urandom > "$T/src/config.json"

now_ms() { date +%s%N | cut -c1-13; }

echo "· 1) proximity: the near replica must serve ~every byte"
./tracker --port 7380 & PIDS+=($!)
sleep 0.3
./maintainer --root "$T/src" --port 7381 --tracker 127.0.0.1:7380 --name near \
    > "$T/near.log" 2>&1 & PIDS+=($!)
LUMABRI_RTT_US=60000 \
./maintainer --root "$T/src" --port 7382 --tracker 127.0.0.1:7380 --name far \
    > "$T/far.log" 2>&1 & PIDS+=($!)
sleep 0.5
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v1" LUMABRI_CACHE="$T/c1" \
    LUMABRI_TRACKER=127.0.0.1:7380 LUMABRI_BLOCK_MIB=1 LUMABRI_PREFETCH=0 \
    ./test_shim "$T/v1" "$T/src"
sleep 6                          # the maintainers print served totals every 5 s
near=$(grep -o 'served [0-9.]* MB' "$T/near.log" | tail -1 | awk '{print $2}' || true)
far=$(grep -o 'served [0-9.]* MB' "$T/far.log" | tail -1 | awk '{print $2}' || true)
near=${near:-0}; far=${far:-0}
echo "   near served $near MB · far served $far MB"
awk -v n="$near" -v f="$far" 'BEGIN { exit !(n + 0 > 20 && f + 0 < 2) }' || {
    echo "   PROXIMITY FAILED: bytes leaked to the far replica"; exit 1; }
echo "   ✓ nearest replica took the traffic"
kill "${PIDS[@]}" 2>/dev/null || true; wait 2>/dev/null || true; PIDS=()

echo "· 2) prefetch: readahead must beat a 40 ms swarm on a cold mirror"
./tracker --port 7380 & PIDS+=($!)
sleep 0.3
LUMABRI_RTT_US=40000 \
./maintainer --root "$T/src" --port 7383 --tracker 127.0.0.1:7380 --name wan \
    > "$T/wan.log" 2>&1 & PIDS+=($!)
sleep 0.5
t0=$(now_ms)
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v2" LUMABRI_CACHE="$T/c2" \
    LUMABRI_TRACKER=127.0.0.1:7380 LUMABRI_BLOCK_MIB=1 LUMABRI_PREFETCH=0 \
    ./test_shim "$T/v2" "$T/src" > /dev/null
t_off=$(( $(now_ms) - t0 ))
t0=$(now_ms)
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v3" LUMABRI_CACHE="$T/c3" \
    LUMABRI_TRACKER=127.0.0.1:7380 LUMABRI_BLOCK_MIB=1 LUMABRI_PREFETCH=4 \
    ./test_shim "$T/v3" "$T/src" > /dev/null
t_on=$(( $(now_ms) - t0 ))
echo "   cold fetch, prefetch off: ${t_off} ms · prefetch 4: ${t_on} ms"
[ "$t_on" -lt $(( t_off * 8 / 10 )) ] || {
    echo "   PREFETCH FAILED: no measurable win"; exit 1; }
echo "   ✓ readahead hides the RTT ($(( (t_off - t_on) * 100 / t_off ))% faster)"
kill "${PIDS[@]}" 2>/dev/null || true; wait 2>/dev/null || true; PIDS=()

echo "· 3) phase-2 failover: kill the busy peer mid-generation"
make -s phase2
MODEL="$PWD/tiny_olmoe"
[ -f "$MODEL/config.json" ] || make -s fixture

SNAP="$MODEL" OMP_NUM_THREADS=6 ./olmoe_p2p 16 8 "$MODEL/ref.json" \
    > "$T/local.out" 2>"$T/local.err" || { cat "$T/local.err"; exit 1; }

# node-a is nearest (no delay) and takes every call; node-b sits 2 ms away
# as the replica that must save the run when node-a dies.
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
    ./expert_node --model "$MODEL" --port 7411 --name node-a & A_PID=$!; PIDS+=($!)
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 LUMABRI_RTT_US=2000 \
    ./expert_node --model "$MODEL" --port 7412 --name node-b & PIDS+=($!)
for p in 7411 7412; do
    for _ in $(seq 1 200); do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; break; }
        sleep 0.2
    done
done

SNAP="$MODEL" OMP_NUM_THREADS=6 LUMABRI_EXPERTS="127.0.0.1:7411,127.0.0.1:7412" \
    ./olmoe_p2p 16 8 "$MODEL/ref.json" > "$T/p2p.out" 2>"$T/p2p.err" & CHAT=$!
# wait for generation to be underway, then kill the peer doing the work
for _ in $(seq 1 100); do
    grep -q "phase 2 active" "$T/p2p.err" 2>/dev/null && break
    sleep 0.1
done
sleep 1.0
kill "$A_PID" 2>/dev/null || true
wait "$CHAT" || { cat "$T/p2p.err"; exit 1; }

A=$(grep "^C engine" "$T/local.out")
B=$(grep "^C engine" "$T/p2p.out")
FO=$(grep -o '[0-9]* failover' "$T/p2p.err" | awk '{print $1}' || true)
FO=${FO:-0}
echo "   failovers survived: $FO"
if [ "$A" = "$B" ] && [ "$FO" -gt 0 ]; then
    echo "   ✓ a peer died mid-generation and not one token changed"
elif [ "$A" != "$B" ]; then
    echo "   FAILOVER FAILED: tokens diverged"; echo "   local: $A"; echo "   p2p:   $B"
    exit 1
else
    echo "   FAILOVER UNPROVEN: the run finished before the kill (0 failovers)"
    exit 1
fi

echo "LUMABRI PHASE 3: PASS"
