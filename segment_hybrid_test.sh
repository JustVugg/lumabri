#!/usr/bin/env bash
# Real execution gate for the central product promise: a Segment executor must
# use resident Expert donors, yet retain its own complete local fallback.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../colibri/c}"
PORT="${PORT:-7890}"
TMP=$(mktemp -d /tmp/lumabri-segment-hybrid.XXXXXX)
PIDS=()
DONOR_PID=""
failed() {
    local status=$?
    echo "SEGMENT HYBRID TEST: FAIL" >&2
    for log in "$TMP"/*.log "$TMP"/*.json; do
        [[ -f "$log" ]] || continue
        echo "--- $(basename "$log")" >&2
        tail -n 160 "$log" >&2 || true
    done
    return "$status"
}
cleanup() {
    kill "${PIDS[@]}" 2>/dev/null || true
    wait "${PIDS[@]}" 2>/dev/null || true
    rm -rf "$TMP"
}
trap failed ERR
trap cleanup EXIT

wait_port() {
    local port=$1
    for _ in $(seq 1 240); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3<&-; exec 3>&-; return 0
        fi
        sleep .05
    done
    return 1
}

prompt=$(python3 - <<'PY'
import json
r=json.load(open('tiny_olmoe/ref.json', encoding='utf-8'))
print(','.join(map(str,r['prompt_ids'])))
PY
)
model_root=$(printf '%064x' 131)
tokenizer_root=$(printf '%064x' 132)

LUMABRI_PEER_KEY="$TMP/tracker.peer.key" \
LUMABRI_KNOWN_HOSTS="$TMP/tracker.known_hosts" \
    ./tracker --port "$PORT" --peer-bindings "$TMP/bindings" \
    >"$TMP/tracker.log" 2>&1 & PIDS+=("$!")
wait_port "$PORT"

LUMABRI_PEER_KEY="$TMP/segment.key" LUMABRI_KNOWN_HOSTS="$TMP/segment.hosts" \
LUMABRI_VERIFY=0 OMP_NUM_THREADS=2 ./segment_node --engine olmoe \
    --model-dir tiny_olmoe --model tiny-olmoe --range 0:16 \
    --port "$((PORT+2))" --tracker "127.0.0.1:$PORT" \
    --advertise "127.0.0.1:$((PORT+2))" --name hybrid-segment \
    --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
    --context 64 --max-rows 16 --sessions 2 --threads 2 --fallback \
    >"$TMP/segment.log" 2>&1 & PIDS+=("$!")
wait_port "$((PORT+2))"

# Establish the oracle with the exact same Segment engine and numeric profile,
# before any Expert donor exists. This detects a network-induced change while
# avoiding assumptions about which tiny fixture a Colibri checkout provides.
OMP_NUM_THREADS=2 ./segment_chat --engine olmoe --model-dir tiny_olmoe \
    --model tiny-olmoe --tracker "127.0.0.1:$PORT" \
    --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
    --prompt-ids "$prompt" --tokens 3 --context 64 --max-rows 16 \
    --retry-first-run --json >"$TMP/baseline.json" 2>"$TMP/baseline.log"
expected=$(python3 - "$TMP/baseline.json" <<'PY'
import json,sys
lines=[line for line in open(sys.argv[1], encoding='utf-8') if line.startswith('{')]
print(','.join(map(str,json.loads(lines[-1])['token_ids'])))
PY
)

# One complete layer is enough: the tracker need not wait for model-wide
# coverage. --resident makes ACTIVE mean that every advertised expert is in
# RAM before the node can receive a single EXEC.
LUMABRI_PEER_KEY="$TMP/donor.key" LUMABRI_KNOWN_HOSTS="$TMP/donor.hosts" \
OMP_NUM_THREADS=2 ./expert_node --model tiny_olmoe --model-name tiny-olmoe \
    --name resident-layer-0 --port "$((PORT+1))" \
    --advertise "127.0.0.1:$((PORT+1))" --tracker "127.0.0.1:$PORT" \
    --layers 0 --resident --parallel 2 >"$TMP/donor.log" 2>&1 &
DONOR_PID=$!; PIDS+=("$DONOR_PID")
wait_port "$((PORT+1))"
sleep 5.2

# The first layer check after a newly joined peer refreshes the immutable
# discovery snapshot. That warm-up may already use later covered layers; the
# measured/oracle run below starts after the new route is fully installed.
OMP_NUM_THREADS=2 ./segment_chat --engine olmoe --model-dir tiny_olmoe \
    --model tiny-olmoe --tracker "127.0.0.1:$PORT" \
    --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
    --prompt-ids "$prompt" --tokens 1 --context 64 --max-rows 16 \
    --retry-first-run --json >"$TMP/discovery.json" 2>"$TMP/discovery.log"

OMP_NUM_THREADS=2 ./segment_chat --engine olmoe --model-dir tiny_olmoe \
    --model tiny-olmoe --tracker "127.0.0.1:$PORT" \
    --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
    --prompt-ids "$prompt" --tokens 3 --expect-ids "$expected" \
    --context 64 --max-rows 16 --retry-first-run >"$TMP/first.log" 2>&1

for _ in $(seq 1 160); do
    ./swarm_probe --tracker "127.0.0.1:$PORT" --model tiny-olmoe >"$TMP/probe.json"
    python3 - "$TMP/probe.json" 2>/dev/null <<'PY' && break || true
import json,sys
rows=json.load(open(sys.argv[1], encoding='utf-8'))['peers']
d=next((r for r in rows if r['name']=='resident-layer-0'), None)
assert d and d['expert']['state']==5 and d['expert']['calls']>0
PY
    sleep .1
done
python3 - "$TMP/probe.json" <<'PY'
import json,sys
rows=json.load(open(sys.argv[1], encoding='utf-8'))['peers']
donor=next((r for r in rows if r['name']=='resident-layer-0'), None)
assert donor, rows
e=donor['expert']
assert e['state']==5 and e['residency_flags'] & 1, e
assert e['resident_count']==8 and e['calls']>0, e
print('SEGMENT HYBRID: resident donor executed', e['calls'], 'calls')
PY

# A dead accelerator may add one failed attempt, never break generation. The
# patched callsite rolls back any partial remote accumulation and executes the
# original local kernel for the whole layer.
kill "$DONOR_PID"
wait "$DONOR_PID" 2>/dev/null || true
DONOR_PID=""
OMP_NUM_THREADS=2 ./segment_chat --engine olmoe --model-dir tiny_olmoe \
    --model tiny-olmoe --tracker "127.0.0.1:$PORT" \
    --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
    --prompt-ids "$prompt" --tokens 3 --expect-ids "$expected" \
    --context 64 --max-rows 16 --retry-first-run >"$TMP/fallback.log" 2>&1

grep -q 'phase 2 partial: 1 of 16 routed layers' "$TMP/segment.log"
grep -Eq 'trying next replica|relay unavailable' "$TMP/segment.log"
echo 'SEGMENT HYBRID TEST: PASS (resident donor used; dead donor falls back locally)'
