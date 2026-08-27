#!/usr/bin/env bash
# Real network gate: every current Colibri family is split across two direct
# Segment peers and its first three greedy tokens must match the independent
# tiny-model oracle. No family is accepted from registration alone.
set -euo pipefail
cd "$(dirname "$0")"

SEGMENT_NODE_BIN=${SEGMENT_NODE_BIN:-./segment_node}
SEGMENT_CHAT_BIN=${SEGMENT_CHAT_BIN:-./segment_chat}

: "${GLM_EDGE_MODEL:?set GLM_EDGE_MODEL}"
: "${GLM_EDGE_REF:?set GLM_EDGE_REF}"
: "${INKLING_EDGE_MODEL:?set INKLING_EDGE_MODEL}"
: "${INKLING_EDGE_REF:?set INKLING_EDGE_REF}"
: "${KIMI_EDGE_MODEL:?set KIMI_EDGE_MODEL}"
: "${KIMI_EDGE_REF:?set KIMI_EDGE_REF}"
: "${OLMOE_EDGE_MODEL:?set OLMOE_EDGE_MODEL}"
: "${OLMOE_EDGE_REF:?set OLMOE_EDGE_REF}"
: "${QWEN_EDGE_MODEL:?set QWEN_EDGE_MODEL}"
: "${QWEN_EDGE_REF:?set QWEN_EDGE_REF}"
: "${DEEPSEEK_EDGE_MODEL:?set DEEPSEEK_EDGE_MODEL}"
: "${DEEPSEEK_EDGE_REF:?set DEEPSEEK_EDGE_REF}"

TMP=$(mktemp -d /tmp/lumabri-segment-direct.XXXXXX)
TRACKER_PID=""
NODE_PIDS=()
cleanup() {
    for pid in "${NODE_PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${NODE_PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
    if [[ -n "$TRACKER_PID" ]]; then
        kill "$TRACKER_PID" 2>/dev/null || true
        wait "$TRACKER_PID" 2>/dev/null || true
    fi
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

env LUMABRI_PEER_KEY="$TMP/tracker.peer.key" \
    LUMABRI_KNOWN_HOSTS="$TMP/tracker.known_hosts" \
    ./tracker --port 7868 --peer-bindings "$TMP/bindings" \
    >"$TMP/tracker.log" 2>&1 &
TRACKER_PID=$!
wait_port 7868

families=(glm inkling kimi olmoe qwen36 deepseek_v4)
models=("$GLM_EDGE_MODEL" "$INKLING_EDGE_MODEL" "$KIMI_EDGE_MODEL" \
        "$OLMOE_EDGE_MODEL" "$QWEN_EDGE_MODEL" "$DEEPSEEK_EDGE_MODEL")
refs=("$GLM_EDGE_REF" "$INKLING_EDGE_REF" "$KIMI_EDGE_REF" \
      "$OLMOE_EDGE_REF" "$QWEN_EDGE_REF" "$DEEPSEEK_EDGE_REF")
layers=(5 8 6 4 8 3)
splits=(2 4 3 2 4 1)

for index in "${!families[@]}"; do
    family=${families[$index]}
    model_dir=${models[$index]}
    ref=${refs[$index]}
    total=${layers[$index]}
    split=${splits[$index]}
    model="tiny-$family"
    model_root=$(printf '%064x' $((index + 1)))
    tokenizer_root=$(printf '%064x' $((index + 17)))
    oracle=$(python3 -c '
import json,sys
family,path=sys.argv[1:]
root=json.load(open(path, encoding="utf-8"))
case=root.get("cases",{}).get("short") if family in ("kimi","deepseek_v4") else root
if not case: raise SystemExit("reference has no short case")
prompt=case["prompt_ids"]
full=case.get("greedy_full_ids", case.get("full_ids"))
if not full or len(full) < len(prompt)+3: raise SystemExit("reference has fewer than three oracle tokens")
print(",".join(map(str,prompt))+"|"+",".join(map(str,full[len(prompt):len(prompt)+3])))
' "$family" "$ref")
    prompt_ids=${oracle%%|*}
    expected=${oracle#*|}

    run_env=(env OMP_NUM_THREADS=2 LUMABRI_PEER_KEY="$TMP/$family.peer.key"
             LUMABRI_KNOWN_HOSTS="$TMP/$family.known_hosts")
    case "$family" in
        glm) run_env+=(GLM_SEGMENT_EBITS=16 GLM_SEGMENT_DBITS=16) ;;
        inkling) run_env+=(INK_SEGMENT_BITS=0) ;;
        kimi) run_env+=(COLI_RAM_OVERCOMMIT=1 K3_BITS=32 K3_MLA_BITS=32
                        K3_HEAD_BITS=32 K3_IDOT=0) ;;
    esac

    "${run_env[@]}" "$SEGMENT_NODE_BIN" --engine "$family" --model-dir "$model_dir" \
        --model "$model" --range "0:$split" --port 7869 \
        --tracker 127.0.0.1:7868 --advertise 127.0.0.1:7869 \
        --name "$family-left" --model-root "$model_root" \
        --tokenizer-root "$tokenizer_root" --context 64 --max-rows 16 \
        --sessions 4 --fallback >"$TMP/$family-left.log" 2>&1 &
    left=$!
    "${run_env[@]}" "$SEGMENT_NODE_BIN" --engine "$family" --model-dir "$model_dir" \
        --model "$model" --range "$split:$total" --port 7870 \
        --tracker 127.0.0.1:7868 --advertise 127.0.0.1:7870 \
        --name "$family-right" --model-root "$model_root" \
        --tokenizer-root "$tokenizer_root" --context 64 --max-rows 16 \
        --sessions 4 --fallback >"$TMP/$family-right.log" 2>&1 &
    right=$!
    NODE_PIDS=("$left" "$right")
    if ! wait_port 7869 || ! wait_port 7870; then
        cat "$TMP/$family-left.log" "$TMP/$family-right.log"
        exit 1
    fi
    if ! "${run_env[@]}" "$SEGMENT_CHAT_BIN" --engine "$family" \
        --model-dir "$model_dir" --model "$model" --tracker 127.0.0.1:7868 \
        --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
        --prompt-ids "$prompt_ids" --tokens 3 --expect-ids "$expected" \
        --context 64 --max-rows 16 --retry-first-run; then
        cat "$TMP/$family-left.log" "$TMP/$family-right.log" "$TMP/tracker.log"
        exit 1
    fi

    # The user-facing TUI drives the same engine through Colibri's
    # SUBMIT/DATA/DONE gateway codec.  Exercise that persistent mode for every
    # family as well as the token-ID oracle above; a single text turn is enough
    # to prove tokenizer, framing, route/session lifecycle and clean EOF.
    serve_output=$(printf 'SUBMIT 1 0 2 2 0.7 0.95\nhi\n' | \
        "${run_env[@]}" "$SEGMENT_CHAT_BIN" --serve --engine "$family" \
        --model-dir "$model_dir" --model "$model" \
        --tracker 127.0.0.1:7868 --model-root "$model_root" \
        --tokenizer-root "$tokenizer_root" --context 64 --max-rows 16)
    if ! grep -q $'\001\001READY\001\001' <<<"$serve_output" ||
       ! grep -q '^DATA 1 ' <<<"$serve_output" ||
       ! grep -q '^DONE 1 STAT ' <<<"$serve_output"; then
        printf '%s\n' "$serve_output"
        cat "$TMP/$family-left.log" "$TMP/$family-right.log"
        echo "SEGMENT DIRECT $family: serve-codec gate failed" >&2
        exit 1
    fi
    if [[ "$family" == olmoe ]]; then
        # A second request must reuse the remote state established by the
        # first one. Keep generation to one token so the committed prefix is
        # exactly the first prompt, then append a new turn and require the
        # executor's explicit reuse diagnostic.
        "${run_env[@]}" python3 - "$SEGMENT_CHAT_BIN" "$model_dir" "$model" \
            "$model_root" "$tokenizer_root" <<'PY'
import subprocess, sys

binary, model_dir, model, model_root, tokenizer_root = sys.argv[1:]
process = subprocess.Popen([
    binary, "--serve", "--engine", "olmoe", "--model-dir", model_dir,
    "--model", model, "--tracker", "127.0.0.1:7868",
    "--model-root", model_root, "--tokenizer-root", tokenizer_root,
    "--context", "64", "--max-rows", "16",
], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

def line():
    value = process.stdout.readline()
    if not value:
        raise RuntimeError("Segment gateway closed unexpectedly")
    return value

if b"READY" not in line() or not line().startswith(b"STAT "):
    raise RuntimeError("Segment gateway did not become ready")

def turn(request_id, prompt):
    header = f"SUBMIT {request_id} 0 {len(prompt)} 1 0.7 0.95\n".encode()
    process.stdin.write(header + prompt + b"\n")
    process.stdin.flush()
    if line() != f"ACCEPT {request_id}\n".encode():
        raise RuntimeError("Segment request was not accepted")
    data_header = line().split()
    if data_header[:2] != [b"DATA", str(request_id).encode()]:
        raise RuntimeError("Segment response has no DATA frame")
    size = int(data_header[2])
    data = process.stdout.read(size)
    if len(data) != size or process.stdout.read(1) != b"\n":
        raise RuntimeError("Segment DATA frame is truncated")
    if not line().startswith(f"DONE {request_id} STAT ".encode()):
        raise RuntimeError("Segment response has no DONE frame")

turn(91, b"hi\n")
turn(92, b"hi\nthere\n")
process.stdin.close()
if process.wait(timeout=15):
    raise RuntimeError("Segment gateway exited with an error")
diagnostics = process.stderr.read().decode("utf-8", "replace")
if "Segment KV reuse:" not in diagnostics:
    raise RuntimeError("the second turn did not reuse remote Segment state")
PY
        client_pids=()
        for client in 1 2; do
            "${run_env[@]}" "$SEGMENT_CHAT_BIN" --engine "$family" \
                --model-dir "$model_dir" --model "$model" \
                --tracker 127.0.0.1:7868 --model-root "$model_root" \
                --tokenizer-root "$tokenizer_root" --prompt-ids "$prompt_ids" \
                --tokens 3 --expect-ids "$expected" --context 64 --max-rows 16 \
                >"$TMP/$family-client-$client.log" 2>&1 &
            client_pids+=("$!")
        done
        clients_ok=1
        for pid in "${client_pids[@]}"; do
            if ! wait "$pid"; then clients_ok=0; fi
        done
        if (( ! clients_ok )); then
            cat "$TMP/$family-client-1.log" "$TMP/$family-client-2.log" \
                "$TMP/$family-left.log" "$TMP/$family-right.log"
            exit 1
        fi
        # A normal compute donor never receives layer numbers from a user.
        # It asks the tracker for the rarest exact origin slice; the selected
        # route must replace that fallback while retaining the other half.
        "${run_env[@]}" "$SEGMENT_NODE_BIN" --engine "$family" \
            --model-dir "$model_dir" --model "$model" --auto-range --port 7871 \
            --tracker 127.0.0.1:7868 --advertise 127.0.0.1:7871 \
            --name "$family-auto" --model-root "$model_root" \
            --tokenizer-root "$tokenizer_root" --context 64 --max-rows 16 \
            --sessions 4 >"$TMP/$family-auto.log" 2>&1 &
        auto=$!
        NODE_PIDS+=("$auto")
        if ! wait_port 7871; then
            cat "$TMP/$family-auto.log" "$TMP/tracker.log"
            exit 1
        fi
        auto_output=$("${run_env[@]}" "$SEGMENT_CHAT_BIN" --engine "$family" \
            --model-dir "$model_dir" --model "$model" \
            --tracker 127.0.0.1:7868 --model-root "$model_root" \
            --tokenizer-root "$tokenizer_root" --prompt-ids "$prompt_ids" \
            --tokens 3 --expect-ids "$expected" --context 64 --max-rows 16)
        if ! grep -q "${family}-auto\[0:${split}\].*${family}-right\[${split}:${total}\](fallback)" \
             <<<"$auto_output"; then
            printf '%s\n' "$auto_output"
            cat "$TMP/$family-auto.log" "$TMP/tracker.log"
            exit 1
        fi
        echo "SEGMENT DIRECT olmoe: PASS (KV reuse + concurrent isolated sessions)"
    fi
    for pid in "${NODE_PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${NODE_PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
    NODE_PIDS=()
    echo "SEGMENT DIRECT $family: PASS (two peers, oracle + TUI codec)"
done

echo "SEGMENT DIRECT: PASS (all six Colibri families)"
