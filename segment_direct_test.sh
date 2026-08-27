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
        --sessions 4 >"$TMP/$family-left.log" 2>&1 &
    left=$!
    "${run_env[@]}" "$SEGMENT_NODE_BIN" --engine "$family" --model-dir "$model_dir" \
        --model "$model" --range "$split:$total" --port 7870 \
        --tracker 127.0.0.1:7868 --advertise 127.0.0.1:7870 \
        --name "$family-right" --model-root "$model_root" \
        --tokenizer-root "$tokenizer_root" --context 64 --max-rows 16 \
        --sessions 4 >"$TMP/$family-right.log" 2>&1 &
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
    if [[ "$family" == olmoe ]]; then
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
        echo "SEGMENT DIRECT olmoe: PASS (two concurrent isolated sessions)"
    fi
    kill "$left" "$right" 2>/dev/null || true
    wait "$left" 2>/dev/null || true
    wait "$right" 2>/dev/null || true
    NODE_PIDS=()
    echo "SEGMENT DIRECT $family: PASS (two peers, three oracle tokens)"
done

echo "SEGMENT DIRECT: PASS (all six Colibri families)"
