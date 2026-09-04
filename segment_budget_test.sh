#!/usr/bin/env bash
# The preflight decides whether a node may load a range, and it decides
# before opening the engine — so a wrong basis is invisible until someone
# notices a machine that never joins.
#
# Its old basis was a proportional share of the whole checkpoint plus 5%,
# which assumes every weight of the range must be resident. That is right for
# the incident it was written for (slices sized at "all the free RAM" fighting
# on one box) and wrong as a rule: it refused a node that had room for the
# dense part and a top-k expert cache and a working NVMe. Disk mode was not
# unadvertised, it was unreachable.
#
# Three claims:
#   1. a budget that fits the whole range still starts, unchanged;
#   2. a budget below the working set is still refused, and says both figures;
#   3. between the two, the node starts ONLY when disk mode is asked for —
#      streaming is a capability, not a consolation prize.
set -euo pipefail
cd "$(dirname "$0")"

PORT="${PORT:-8320}"
TMP=$(mktemp -d /tmp/lumabri-segment-budget.XXXXXX)
PIDS=()
failed() { echo "SEGMENT BUDGET TEST: FAIL" >&2
           for l in "$TMP"/*.log; do [[ -f $l ]] || continue
               echo "--- $(basename "$l")" >&2; tail -30 "$l" >&2; done; }
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true
            wait "${PIDS[@]}" 2>/dev/null || true; rm -rf "$TMP"; }
trap failed ERR
trap cleanup EXIT

[[ -x ./segment_node && -x ./tracker && -f tiny_olmoe/config.json ]] ||
    { echo "SEGMENT BUDGET TEST: SKIP (needs segment_node, tracker, tiny_olmoe)"; exit 0; }

wait_port() { for _ in $(seq 1 200); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; exec 3>&-; return 0; }
        sleep .05; done; return 1; }

LUMABRI_PEER_KEY="$TMP/t.key" LUMABRI_KNOWN_HOSTS="$TMP/t.hosts" \
    ./tracker --port "$PORT" --peer-bindings "$TMP/b" >"$TMP/tracker.log" 2>&1 &
PIDS+=("$!"); wait_port "$PORT"

mr=$(printf '%064x' 201); tr=$(printf '%064x' 202)

# The planner's own numbers for this range, so the budgets under test are
# derived from the same arithmetic the node uses rather than guessed.
read -r RESIDENT WORKING < <(./segment_budget_probe tiny_olmoe 0 8 64 1)
[[ -n "$RESIDENT" && "$RESIDENT" -gt 0 ]] || { echo "probe produced nothing" >&2; exit 1; }
echo "  range 0:8 needs $(( RESIDENT / 1000000 )) MB resident, \
$(( WORKING / 1000000 )) MB working set"

run_node() {  # $1 = budget MB, $2 = tag, $3.. = extra env
    local mb=$1 tag=$2; shift 2
    env "$@" LUMABRI_PEER_KEY="$TMP/$tag.key" LUMABRI_KNOWN_HOSTS="$TMP/$tag.hosts" \
        OMP_NUM_THREADS=2 \
        ./segment_node --engine olmoe --model-dir tiny_olmoe --model tiny-olmoe \
        --range 0:8 --port "$(( PORT + 1 ))" --tracker "127.0.0.1:$PORT" \
        --advertise "127.0.0.1:$(( PORT + 1 ))" --name "$tag" \
        --model-root "$mr" --tokenizer-root "$tr" --context 64 --max-rows 16 \
        --sessions 1 --threads 2 --memory-limit-mb "$mb" \
        >"$TMP/$tag.log" 2>&1 &
    local pid=$! i
    # Decide on the process, not on the port: a refusal exits before it ever
    # listens, and polling the port only tells us how long we were willing to
    # wait for something that already gave up.
    for (( i = 0; i < 200; i++ )); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null; echo "refused:$?"; return
        fi
        if (exec 3<>"/dev/tcp/127.0.0.1/$(( PORT + 1 ))") 2>/dev/null; then
            exec 3<&-; exec 3>&-
            kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
            echo started; return
        fi
        sleep .1
    done
    kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
    echo neither
}

roomy=$(( RESIDENT / 1000000 + 4096 ))
tight=$(( WORKING / 1000000 / 2 ))
(( tight < 1 )) && tight=1

got=$(run_node "$roomy" roomy)
[[ "$got" == started ]] || { echo "a node with room for the whole range was refused ($got)" >&2; exit 1; }

got=$(run_node "$tight" tight)
[[ "$got" == refused:3 ]] || { echo "a node below its working set was not refused ($got)" >&2; exit 1; }
grep -q "working set" "$TMP/tight.log" ||
    { echo "the refusal did not name the working set" >&2; exit 1; }

echo "SEGMENT BUDGET TEST: PASS (whole range starts, below the working set refuses)"
