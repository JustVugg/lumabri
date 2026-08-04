#!/usr/bin/env bash
# lumibri phase 2 — an HONEST benchmark on a single machine.
#
# Yesterday's run proved correctness but its tok/s meant little: peers and
# chatter fought over the same cores, and localhost has no network at all.
# This script closes both gaps as far as one box allows.
#
#  1. CORE PARTITION. The chatter is pinned to physical cores 0-2 and each
#     peer to its own physical core (3,4,5) via taskset. No peer ever steals
#     a cycle from the chatter, which is exactly what separate machines give
#     you — and the chatter is deliberately given FEWER cores than it had, so
#     the comparison is not flattered.
#
#  2. THE RIGHT BASELINE. Comparing P2P against experts already resident in
#     RAM answers a question nobody has: if the model fit in RAM you would not
#     need lumibri. So three runs, not two:
#       A  RAM-resident  — the unattainable best case (small models only)
#       B  disk-streamed — CACHE=1 + EXPERT_DROP=1 (fadvise DONTNEED after
#                          every read), which is what a 167 GB model does
#       C  P2P           — experts live and run on peers
#     B is the baseline lumibri actually has to beat.
#
#  3. NETWORK REALISM. If a netem qdisc is present on `lo`, its delay is
#     reported. Adding it needs root, so the script never does it silently:
#         sudo tc qdisc add dev lo root netem delay 0.25ms 0.05ms
#         sudo tc qdisc del dev lo root          # to undo
#
# Everything still runs on one machine, so this is a bound, not a promise:
# see RESULTS_PHASE2.md for what remains unproven.
set -euo pipefail
cd "$(dirname "$0")"

MODEL="${MODEL:-$PWD/olmoe_bench}"
PRESET="${PRESET:-real}"
NODES="${NODES:-3}"
PORT0="${PORT0:-7501}"
CHATTER_CPUS="${CHATTER_CPUS:-0,1,2,3,4,5}"   # physical cores 0-2 (with SMT siblings)
PEER_CPUS=("6,7" "8,9" "10,11")               # one physical core per peer

make -s phase2
[ -f "$MODEL/config.json" ] || python3 make_tiny_olmoe.py "$MODEL" "$PRESET"

NETEM=$(tc qdisc show dev lo 2>/dev/null | grep -o 'netem.*' || true)
echo
if [ -n "$NETEM" ]; then echo "rete emulata su lo: $NETEM"
else echo "rete: localhost puro (nessun netem — vedi l'intestazione dello script)"; fi
echo "chatter su CPU $CHATTER_CPUS · $NODES peer, un core fisico ciascuno"

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; }
trap cleanup EXIT

run_engine() {   # run_engine <label> <cache> <drop> <experts-env>
    local label=$1 cache=$2 drop=$3 experts=$4
    # env, not a bare prefix: an assignment produced by an expansion is not
    # recognised as one by the shell and would be run as a command.
    env SNAP="$MODEL" OMP_NUM_THREADS=6 EXPERT_DROP="$drop" \
        ${experts:+LUMIBRI_EXPERTS="$experts"} \
        taskset -c "$CHATTER_CPUS" ./olmoe_p2p "$cache" 8 "$MODEL/ref.json" \
        > "$label.out" 2> "$label.err" || { cat "$label.err"; exit 1; }
}

echo
echo "══ A) locale, esperti residenti in RAM (caso migliore irraggiungibile)"
run_engine bench_a 32 0 ""
grep -E "^Speed|hit rate" bench_a.out

echo
echo "══ B) locale, esperti streammati dal disco (il caso vero su un modello grande)"
run_engine bench_b 1 1 ""
grep -E "^Speed|hit rate" bench_b.out

start_peers() {   # start_peers <rtt_us> <jitter_us> <loss_ppm>
    ADDRS=""
    for i in $(seq 0 $((NODES-1))); do
        p=$((PORT0+i))
        env OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
            LUMIBRI_RTT_US="$1" LUMIBRI_JITTER_US="$2" LUMIBRI_LOSS_PPM="$3" \
            taskset -c "${PEER_CPUS[$i]}" ./expert_node --model "$MODEL" --port "$p" \
                    --name "node-$i" --stride "$NODES:$i" >/dev/null 2>&1 & PIDS+=($!)
        ADDRS="${ADDRS:+$ADDRS,}127.0.0.1:$p"
    done
    for i in $(seq 0 $((NODES-1))); do
        p=$((PORT0+i))
        for _ in $(seq 1 400); do
            (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; break; }
            sleep 0.25
        done
    done
}

stop_peers() { kill "${PIDS[@]}" 2>/dev/null || true; wait 2>/dev/null || true; PIDS=(); sleep 0.5; }

# label · rtt_us · jitter_us · loss_ppm · human name
SCENARIOS=(
  "c_ideal:0:0:0:localhost puro (nessuna rete emulata)"
  "c_lan:250:50:0:LAN gigabit — 0.25 ms ± 0.05"
  "c_wan:30000:5000:1000:Internet — 30 ms ± 5, 0.1% di perdita"
)

for sc in "${SCENARIOS[@]}"; do
    IFS=: read -r lab rtt jit loss human <<< "$sc"
    echo
    echo "══ C/$lab) P2P — $human"
    start_peers "$rtt" "$jit" "$loss"
    run_engine "bench_$lab" 1 0 "$ADDRS"
    grep -E "^Speed" "bench_$lab.out"
    grep -E "^\[lumibri\] [0-9]" "bench_$lab.err" || true
    stop_peers
done

echo
A=$(grep "^C engine" bench_a.out); B=$(grep "^C engine" bench_b.out)
BAD=0
[ "$A" = "$B" ] || BAD=1
for sc in "${SCENARIOS[@]}"; do
    lab=${sc%%:*}
    [ "$(grep '^C engine' "bench_$lab.out")" = "$A" ] || BAD=1
done
if [ $BAD -eq 0 ]; then
    echo "✓ tutti i percorsi — locale, disco e P2P a ogni latenza — danno gli STESSI token"
else
    echo "✗ DIVERGENZA fra i percorsi"; exit 1
fi

python3 - <<'EOF'
import re
def get(f, pat):
    m = re.search(pat, open(f).read(), re.M)
    return float(m.group(1)) if m else float("nan")
def speed(f): return get(f, r"^Speed: ([\d.]+)")
def rss(f):   return get(f, r"PEAK RSS: ([\d.]+)")
def round_ms(f):
    return get(f, r"\(([\d.]+) ms per layer round\)")

rows = [("A  locale, esperti in RAM", "bench_a", ""),
        ("B  locale, esperti dal disco", "bench_b", "  <- il baseline vero"),
        ("C  P2P, localhost", "bench_c_ideal", ""),
        ("C  P2P, LAN 0.25 ms", "bench_c_lan", ""),
        ("C  P2P, WAN 30 ms", "bench_c_wan", "")]
b = speed("bench_b.out")
print()
for name, f, note in rows:
    s, r = speed(f + ".out"), rss(f + ".out")
    lr = round_ms(f + ".err")
    extra = f"   round {lr:.2f} ms" if lr == lr else ""
    print(f"  {name:32s} {s:6.2f} tok/s   RSS {r:.2f} GB{extra}{note}")
print()
for name, f, _ in rows[2:]:
    print(f"  {name:32s} = {speed(f+'.out')/b:6.1f}x il disco")
EOF
