#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ENGINE="${ENGINE:-../colibri/c}"
PORT="${PORT:-7870}"
T=$(mktemp -d /tmp/lumabri-resident.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; wait "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 200); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; exec 3>&-; return; }
        sleep .05
    done
    return 1
}
make -s tracker swarm_probe expert_node fixture ENGINE="$ENGINE"
LUMABRI_PEER_BINDINGS="$T/bindings" ./tracker --port "$PORT" >"$T/tracker.log" 2>&1 & PIDS+=("$!")
wait_port "$PORT"
LUMABRI_PEER_KEY="$T/donor.key" OMP_NUM_THREADS=1 ./expert_node \
    --model tiny_olmoe --model-name tiny_olmoe --name resident-a \
    --port "$((PORT+1))" --advertise "127.0.0.1:$((PORT+1))" \
    --tracker "127.0.0.1:$PORT" --hold 4 --resident >"$T/donor.log" 2>&1 & PIDS+=("$!")
wait_port "$((PORT+1))"
for _ in $(seq 1 160); do
    ./swarm_probe --tracker "127.0.0.1:$PORT" --model tiny_olmoe >"$T/probe.json"
    python3 - "$T/probe.json" 2>/dev/null <<'PY' && break || true
import json,sys
p=json.load(open(sys.argv[1]))['peers']
assert len(p)==1
e=p[0]['expert']
assert e['state']==5 and e['residency_flags'] & 1
assert e['resident_count']==4 and e['resident_ram_bytes']>0
PY
    sleep .1
done
python3 - "$T/probe.json" <<'PY'
import json,sys
e=json.load(open(sys.argv[1]))['peers'][0]['expert']
assert e['state']==5 and e['resident_count']==4 and e['resident_ram_bytes']>0, e
print('RESIDENT HOLD TEST: PASS', e)
PY
