#!/usr/bin/env bash
# lumabri phase 4 — bootstrap-and-delegate, verified. Three claims:
#
#   1) SSD CACHE   an expert node with a tiny LRU (--cache 8, forced
#                  evictions) streams experts from disk with the engine's
#                  own loader: tokens must be IDENTICAL to the local run.
#   2) BOOTSTRAP   no LUMABRI_EXPERTS anywhere: the chatter discovers the
#                  server's executor from the tracker and runs phase 2
#                  against it. A fresh swarm works from minute zero.
#   3) DELEGATE    a nearer donor holding half the experts joins; it wins
#                  its calls. Killed mid-generation, the run falls back to
#                  the server and finishes with identical tokens.
set -euo pipefail
cd "$(dirname "$0")"

make -s all phase2
MODEL="$PWD/tiny_olmoe"
[ -f "$MODEL/config.json" ] || make -s fixture

T=$(mktemp -d /tmp/lumabri-phase4.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

SNAP="$MODEL" OMP_NUM_THREADS=6 ./olmoe_p2p 16 8 "$MODEL/ref.json" \
    > "$T/local.out" 2>"$T/local.err" || { cat "$T/local.err"; exit 1; }
REF=$(grep "^C engine" "$T/local.out")

wait_port() {
    for _ in $(seq 1 200); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.2
    done
    return 1
}

echo "· 1) SSD cache: --cache 8 on 128 experts (evictions guaranteed)"
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
    ./expert_node --model "$MODEL" --port 7431 --name ssd-node --cache 8 \
    > "$T/ssd.log" 2>&1 & PIDS+=($!)
wait_port 7431
SNAP="$MODEL" OMP_NUM_THREADS=6 LUMABRI_EXPERTS="127.0.0.1:7431" \
    ./olmoe_p2p 16 8 "$MODEL/ref.json" > "$T/ssd.out" 2>"$T/ssd.err" \
    || { cat "$T/ssd.err"; exit 1; }
sleep 6                                   # let the node print its stats line
B=$(grep "^C engine" "$T/ssd.out")
[ "$REF" = "$B" ] || { echo "   SSD CACHE FAILED: tokens diverged"; exit 1; }
STATS=$(grep -o '[0-9]* exec calls · [0-9]* cold loads · [0-9.]*% RAM hit' "$T/ssd.log" | tail -1 || true)
echo "   ✓ identical tokens, experts streamed from disk ($STATS)"
kill "${PIDS[@]}" 2>/dev/null || true; wait 2>/dev/null || true; PIDS=()

echo "· 2) bootstrap: chatter discovers the server's executor via tracker"
./tracker --port 7430 & PIDS+=($!)
sleep 0.3
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
    ./expert_node --model "$MODEL" --port 7432 --name srv-exec --cache 32 \
                  --tracker 127.0.0.1:7430 > "$T/srv.log" 2>&1 & SRV_PID=$!; PIDS+=($!)
wait_port 7432
sleep 1                                    # first EREG heartbeat
SNAP="$MODEL" OMP_NUM_THREADS=6 \
    LUMABRI_TRACKER=127.0.0.1:7430 LUMABRI_MODEL=tiny_olmoe \
    ./olmoe_p2p 16 8 "$MODEL/ref.json" > "$T/boot.out" 2>"$T/boot.err" \
    || { cat "$T/boot.err"; exit 1; }
B=$(grep "^C engine" "$T/boot.out")
grep -q "discovered" "$T/boot.err" || { echo "   BOOTSTRAP FAILED: no discovery"; exit 1; }
[ "$REF" = "$B" ] || { echo "   BOOTSTRAP FAILED: tokens diverged"; exit 1; }
echo "   ✓ zero configuration: tracker → server executor → identical tokens"

echo "· 3) delegate & fall back: nearer donor joins, then dies mid-run"
# the server sits 3 ms away; the donor holds HALF the experts at 0 ms and
# must win those calls while it lives
kill "$SRV_PID" 2>/dev/null || true
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 LUMABRI_RTT_US=3000 \
    ./expert_node --model "$MODEL" --port 7433 --name srv-far --cache 32 \
                  --tracker 127.0.0.1:7430 > "$T/far.log" 2>&1 & PIDS+=($!)
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
    ./expert_node --model "$MODEL" --port 7434 --name donor --stride 2:0 \
                  --tracker 127.0.0.1:7430 > "$T/donor.log" 2>&1 & DONOR_PID=$!; PIDS+=($!)
wait_port 7433; wait_port 7434
sleep 1
SNAP="$MODEL" OMP_NUM_THREADS=6 \
    LUMABRI_TRACKER=127.0.0.1:7430 LUMABRI_MODEL=tiny_olmoe \
    ./olmoe_p2p 16 8 "$MODEL/ref.json" > "$T/del.out" 2>"$T/del.err" & CHAT=$!
for _ in $(seq 1 100); do
    grep -q "phase 2 active" "$T/del.err" 2>/dev/null && break
    sleep 0.1
done
sleep 1.0
kill "$DONOR_PID" 2>/dev/null || true
wait "$CHAT" || { cat "$T/del.err"; exit 1; }
B=$(grep "^C engine" "$T/del.out")
FO=$(grep -o '[0-9]* failover' "$T/del.err" | awk '{print $1}' || true)
FO=${FO:-0}
# the donor was in use iff the failover messages name its address: the
# chatter only fails over FROM a peer it was actively calling
DONOR_USED=$(grep -c "127.0.0.1:7434 failed" "$T/del.err" || true)
echo "   failovers away from the dead donor: $DONOR_USED · total failovers: $FO"
[ "$REF" = "$B" ] || { echo "   DELEGATE FAILED: tokens diverged"; exit 1; }
[ "$FO" -gt 0 ] || { echo "   DELEGATE UNPROVEN: no failover happened"; exit 1; }
[ "$DONOR_USED" -gt 0 ] || { echo "   DELEGATE UNPROVEN: donor never used"; exit 1; }
echo "   ✓ the swarm delegated to the donor, lost it, fell back to the server —"
echo "     and not one token changed"

echo "LUMABRI PHASE 4: PASS"
