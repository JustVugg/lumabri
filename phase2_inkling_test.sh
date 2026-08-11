#!/usr/bin/env bash
# lumabri phase 2 on the Inkling engine — the same experiment as
# phase2_test.sh, against inkling.c.
#
# Inkling's shape: dense layers up front (c->sparse[]), gate and up fused
# into one 2·I-row matmul, and shared experts that must keep running on the
# chatter. Experts here are f32 (bits=0) so the fixture needs no container
# packing — the P2P question is where the expert runs, not how it is
# quantized, and the peer must be told the same bits or it holds different
# weights.
#
# The tokens must be IDENTICAL.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../moe-stream/c}"
NODES="${NODES:-2}"
PORT0="${PORT0:-7481}"   # clear of phase5 (745x) and sign_test (746x)
CAP="${CAP:-4}"
BITS=0

make -s expert_node_inkling inkling_p2p ENGINE="$ENGINE"

T=$(mktemp -d /tmp/lumabri-ink.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

MODEL="${MODEL:-$T/tiny_inkling}"
if [ ! -f "$MODEL/config.json" ]; then
    python3 make_tiny_inkling.py "$MODEL" > /dev/null
fi
# The ref harness is how this engine runs a fixed-length greedy generation.
# Its oracle values start as placeholders and are then REPLACED by the local
# run's own tokens, so the P2P run is scored against local by the engine
# itself: "Matching tokens: 16/16" and exit 0 is the engine agreeing, not us
# comparing strings.
python3 - "$T/ref.json" <<'EOF'
import json, sys
p = [3, 7, 11, 19, 23, 29, 31, 37]
json.dump({"prompt_ids": p, "full_ids": p + [0] * 16}, open(sys.argv[1], "w"))
EOF

run() { SNAP="$MODEL" OMP_NUM_THREADS="${THREADS:-2}" "$@" \
        LUMABRI_EXPERT_BITS="$BITS" ./inkling_p2p "$CAP" "$BITS" "$T/ref.json"; }

echo
echo "══ THREAD POLICY — default Inkling node uses physical cores, not SMT threads"
POLICY_PORT=$((PORT0+60))
env -u OMP_NUM_THREADS -u OMP_PROC_BIND -u COLI_NO_OMP_TUNE -u COLI_OMP_TUNED \
    ./expert_node_inkling --model "$MODEL" --port "$POLICY_PORT" --name ink-policy \
                          --bits "$BITS" --cache 1 > "$T/policy.log" 2>&1 & POLICY_PID=$!; PIDS+=($!)
for _ in $(seq 1 300); do
    (exec 3<>/dev/tcp/127.0.0.1/$POLICY_PORT) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.1
done
grep -q "applying the engine hot-thread policy" "$T/policy.log" || {
    echo "Inkling node did not apply its OpenMP hot-thread policy"; cat "$T/policy.log"; exit 1; }
POLICY_LINE=$(grep "thread.* each, .* physical core" "$T/policy.log" | tail -1)
PT=$(printf '%s\n' "$POLICY_LINE" | sed -nE 's/.* ([0-9]+) threads? each, ([0-9]+) physical cores?.*/\1/p')
PC=$(printf '%s\n' "$POLICY_LINE" | sed -nE 's/.* ([0-9]+) threads? each, ([0-9]+) physical cores?.*/\2/p')
[ -n "$PT" ] && [ -n "$PC" ] && [ "$PT" -le "$PC" ] || {
    echo "Inkling node oversubscribed its physical cores: $POLICY_LINE"; exit 1; }
kill "$POLICY_PID" 2>/dev/null || true
wait "$POLICY_PID" 2>/dev/null || true
echo "✓ Inkling default: $PT OpenMP threads on $PC available physical cores"

echo
echo "══ A) LOCAL — experts read and run by the engine itself"
run env > "$T/local.out" 2>"$T/local.err" || true   # exit 1 = "differs from oracle"
grep -qE "^C engine" "$T/local.out" || { cat "$T/local.out" "$T/local.err"; exit 1; }
grep -E "^C engine" "$T/local.out"
# the local tokens become the oracle the engine scores the P2P run against
python3 - "$T/ref.json" "$T/local.out" <<'EOF'
import json, re, sys
ref = json.load(open(sys.argv[1]))
line = [l for l in open(sys.argv[2]) if l.startswith("C engine")][0]
ref["full_ids"] = ref["prompt_ids"] + [int(t) for t in re.findall(r"\d+", line.split(":")[1])]
json.dump(ref, open(sys.argv[1], "w"))
EOF

echo
echo "══ starting $NODES expert peers (each holds 1/$NODES of the experts)"
ADDRS=""
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    OMP_NUM_THREADS="${NODE_THREADS:-2}" \
    ./expert_node_inkling --model "$MODEL" --port "$p" --name "ink-$i" \
                          --bits "$BITS" --stride "$NODES:$i" \
                          > "$T/node$i.log" 2>&1 & PIDS+=($!)
    ADDRS="${ADDRS:+$ADDRS,}127.0.0.1:$p"
done
for i in $(seq 0 $((NODES-1))); do
    p=$((PORT0+i))
    for _ in $(seq 1 300); do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; break; }
        sleep 0.2
    done
done
grep -h "dense and route nothing\|holding" "$T/node0.log" || true

echo
echo "══ B) P2P — every routed expert runs on a peer, scored against the local run"
RC=0
run env LUMABRI_EXPERTS="$ADDRS" > "$T/p2p.out" 2>"$T/p2p.err" || RC=$?
grep -qE "^C engine" "$T/p2p.out" || { cat "$T/p2p.out" "$T/p2p.err"; exit 1; }
grep -E "^C engine|^Matching" "$T/p2p.out"
grep -E "^\[lumabri\]" "$T/p2p.err" || true

echo
A=$(grep "^C engine" "$T/local.out")
B=$(grep "^C engine" "$T/p2p.out")
if [ "$A" = "$B" ] && [ "$RC" -eq 0 ]; then
    echo "✓ IDENTICI — la rete non ha cambiato un solo token"
    echo "  $A"
else
    echo "✗ DIVERGENZA — il P2P ha cambiato l'output:"
    echo "  local: $A"
    echo "  p2p  : $B"
    exit 1
fi
