#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
PORT=${PORT:-7900}
TMP=$(mktemp -d /tmp/lumabri-governor.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; wait "${PIDS[@]}" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 240); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null &&
            { exec 3<&-; exec 3>&-; return 0; }
        sleep .05
    done
    return 1
}

export LUMABRI_GOVERNOR_FILE="$TMP/governor.state"
./test_machine
./lumabri machine --json >"$TMP/machine.json"
python3 - "$TMP/machine.json" <<'PY'
import json,sys
p=json.load(open(sys.argv[1], encoding='utf-8'))
assert p['schema']==1 and p['cpu']['physical']>0
assert p['memory']['total']>p['memory']['available']>0
assert p['gpu']['count']>=0 and p['disk_available']>0
PY

LUMABRI_PEER_KEY="$TMP/tracker.key" ./tracker --port "$PORT" \
    --peer-bindings "$TMP/bindings" >"$TMP/tracker.log" 2>&1 & PIDS+=("$!")
wait_port "$PORT"
LUMABRI_PEER_KEY="$TMP/expert.key" OMP_NUM_THREADS=1 ./expert_node \
    --model tiny_olmoe --model-name governor-tiny --name governed-expert \
    --port "$((PORT+1))" --advertise "127.0.0.1:$((PORT+1))" \
    --tracker "127.0.0.1:$PORT" --layers 0 --resident --parallel 1 \
    >"$TMP/expert.log" 2>&1 & PIDS+=("$!")
wait_port "$((PORT+1))"
root=$(printf '%064x' 201)
tokenizer=$(printf '%064x' 202)
LUMABRI_PEER_KEY="$TMP/segment.key" OMP_NUM_THREADS=1 ./segment_node \
    --engine olmoe --model-dir tiny_olmoe --model governor-tiny \
    --range 0:16 --port "$((PORT+2))" --advertise "127.0.0.1:$((PORT+2))" \
    --tracker "127.0.0.1:$PORT" --name governed-segment \
    --model-root "$root" --tokenizer-root "$tokenizer" --context 64 \
    --max-rows 16 --sessions 1 --threads 1 >"$TMP/segment.log" 2>&1 & PIDS+=("$!")
wait_port "$((PORT+2))"

wait_state() {
    local wanted=$1 count=$2
    for _ in $(seq 1 240); do
        ./swarm_probe --tracker "127.0.0.1:$PORT" --model governor-tiny \
            >"$TMP/probe.json" || true
        python3 - "$TMP/probe.json" "$wanted" "$count" 2>/dev/null <<'PY' && return 0 || true
import json,sys
rows=json.load(open(sys.argv[1], encoding='utf-8'))['peers']
row=next((r for r in rows if r['name']=='governed-expert'), None)
assert row and row['expert']['state']==int(sys.argv[2])
assert row['expert']['count']==int(sys.argv[3])
PY
        sleep .1
    done
    cat "$TMP/expert.log" "$TMP/tracker.log" "$TMP/probe.json" >&2
    return 1
}

wait_segment_flag() {
    local draining=$1
    for _ in $(seq 1 240); do
        ./swarm_probe --tracker "127.0.0.1:$PORT" --model governor-tiny \
            >"$TMP/probe.json" || true
        python3 - "$TMP/probe.json" "$draining" 2>/dev/null <<'PY' && return 0 || true
import json,sys
rows=json.load(open(sys.argv[1], encoding='utf-8'))['peers']
row=next((r for r in rows if r['name']=='governed-segment'), None)
assert row
assert bool(row['segment']['flags'] & 1)==bool(int(sys.argv[2]))
PY
        sleep .1
    done
    cat "$TMP/segment.log" "$TMP/probe.json" >&2
    return 1
}

wait_state 5 8
wait_segment_flag 0
./lumabri pause >/dev/null
wait_state 6 0
wait_segment_flag 1
./lumabri resume >/dev/null
wait_state 5 8
wait_segment_flag 0
grep -q 'governor ACTIVE -> PAUSED' "$TMP/expert.log"
grep -q 'governor PAUSED -> RECOVERY' "$TMP/expert.log"
grep -q 'governor RECOVERY -> ACTIVE' "$TMP/expert.log"
grep -q 'governor ACTIVE -> PAUSED' "$TMP/segment.log"
grep -q 'governor PAUSED -> RECOVERY' "$TMP/segment.log"
grep -q 'governor RECOVERY -> ACTIVE' "$TMP/segment.log"
echo 'MACHINE GOVERNOR TEST: PASS (profile, Expert zero coverage, Segment drain, recovery)'
