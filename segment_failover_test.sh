#!/usr/bin/env bash
# Kill the selected non-fallback executor after turn one. Turn two must restore
# every range at the common checkpoint, replace the dead range with its exact
# origin replica and replay the token delta without ending the conversation.
set -euo pipefail
cd "$(dirname "$0")"
: "${OLMOE_EDGE_MODEL:?set OLMOE_EDGE_MODEL}"
SEGMENT_NODE_BIN=${SEGMENT_NODE_BIN:-./segment_node}
SEGMENT_CHAT_BIN=${SEGMENT_CHAT_BIN:-./segment_chat}
TRACKER_BIN=${TRACKER_BIN:-./tracker}
TMP=$(mktemp -d /tmp/lumabri-segment-failover.XXXXXX)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$TMP"
}
trap cleanup EXIT
wait_port() {
    local port=$1
    for _ in $(seq 1 200); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3<&-; exec 3>&-; return 0
        fi
        sleep 0.025
    done
    return 1
}
env LUMABRI_PEER_KEY="$TMP/tracker.key" LUMABRI_KNOWN_HOSTS="$TMP/known" \
    "$TRACKER_BIN" --port 8068 --peer-bindings "$TMP/bindings" \
    >"$TMP/tracker.log" 2>&1 &
PIDS+=("$!"); wait_port 8068
root=$(printf '%064x' 101); tok=$(printf '%064x' 102)
common=(env OMP_NUM_THREADS=2 LUMABRI_PEER_KEY="$TMP/node.key"
        LUMABRI_KNOWN_HOSTS="$TMP/known")
start_node() {
    local range=$1 port=$2 name=$3 fallback=$4
    "${common[@]}" "$SEGMENT_NODE_BIN" --engine olmoe \
        --model-dir "$OLMOE_EDGE_MODEL" --model tiny-failover \
        --range "$range" --port "$port" --tracker 127.0.0.1:8068 \
        --advertise "127.0.0.1:$port" --name "$name" \
        --model-root "$root" --tokenizer-root "$tok" --context 64 \
        --max-rows 16 --sessions 4 $fallback >"$TMP/$name.log" 2>&1 &
    LAST_PID=$!; PIDS+=("$LAST_PID"); wait_port "$port"
}
start_node 0:2 8069 origin-left --fallback
start_node 2:4 8070 origin-right --fallback
start_node 0:2 8071 donor-left ""
DONOR_PID=$LAST_PID
sleep 1

env OMP_NUM_THREADS=2 LUMABRI_PEER_KEY="$TMP/client.key" \
    LUMABRI_KNOWN_HOSTS="$TMP/known" LUMABRI_SAMPLE_SEED=11 \
    python3 - "$DONOR_PID" "$SEGMENT_CHAT_BIN" "$OLMOE_EDGE_MODEL" "$root" "$tok" <<'PY'
import os, subprocess, sys, time
donor, binary, model_dir, root, tok = sys.argv[1:]
p = subprocess.Popen([
    binary, "--serve", "--engine", "olmoe",
    "--model-dir", model_dir, "--model", "tiny-failover",
    "--tracker", "127.0.0.1:8068", "--model-root", root,
    "--tokenizer-root", tok, "--context", "64", "--max-rows", "16",
], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def line():
    value = p.stdout.readline()
    if not value: raise RuntimeError("Segment gateway closed")
    return value
if b"READY" not in line() or not line().startswith(b"STAT "):
    raise RuntimeError("gateway not ready")
def turn(rid, prompt):
    p.stdin.write(f"SUBMIT {rid} 0 {len(prompt)} 1 0.7 0.95\n".encode()+prompt+b"\n")
    p.stdin.flush()
    if line() != f"ACCEPT {rid}\n".encode(): raise RuntimeError("not accepted")
    seen_data = False
    while True:
        frame = line()
        if frame.startswith(f"DATA {rid} ".encode()):
            size=int(frame.split()[2]); p.stdout.read(size)
            if p.stdout.read(1) != b"\n": raise RuntimeError("truncated data")
            seen_data = True
        elif frame.startswith(f"DONE {rid} STAT ".encode()):
            break
        elif not frame.startswith(f"PROGRESS {rid} ".encode()):
            raise RuntimeError("unexpected frame: "+repr(frame))
    if not seen_data: raise RuntimeError("no streamed data")
turn(1, b"hi\n")
os.kill(int(donor), 15)
time.sleep(.3)
turn(2, b"hi\nthere\n")
p.stdin.close()
if p.wait(timeout=30): raise RuntimeError("gateway failed")
diagnostics=p.stderr.read().decode("utf-8", "replace")
if "Segment failover: restored checkpoint" not in diagnostics:
    raise RuntimeError("checkpoint failover did not run:\n"+diagnostics)
PY
echo "SEGMENT FAILOVER: PASS (checkpoint restore + replay after peer death)"
