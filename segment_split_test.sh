#!/usr/bin/env bash
# Step 0 of the roadmap: the truth about the Segment path.
#
# Two questions, and the second one is the product.
#
#   1. Does splitting change the model? The same greedy prompt has to produce
#      the same token ids whether one node holds every layer or several hold
#      a range each. Anything else and there is no product to optimise.
#
#   2. What does splitting COST? Layers are sequential: node B cannot start
#      before node A's output arrives, so twelve layers on one machine take
#      about as long as six plus six on two, plus a hop. This measures that
#      hop instead of assuming it, at equal total threads so the comparison
#      isolates the structure rather than the core count.
#
# Relay is forbidden throughout: every node advertises a direct address, and
# a run that fell back to the tracker tunnel would measure the tunnel.
#
# On one machine this is a loopback rehearsal — real LAN numbers need real
# machines, and LUMABRI_SPLIT_HOSTS is where those go. What it proves here is
# the invariance, which is machine-independent, and the shape of the cost.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../colibri/c}"
PORT="${PORT:-7920}"
MODEL_DIR="${SPLIT_MODEL_DIR:-tiny_olmoe}"
ENGINE_ID="${SPLIT_ENGINE:-olmoe}"
MODEL_NAME="${SPLIT_MODEL:-tiny-olmoe}"
TOKENS="${SPLIT_TOKENS:-8}"
THREADS_TOTAL="${SPLIT_THREADS:-4}"
CONTEXT="${SPLIT_CONTEXT:-64}"
ROUNDS="${SPLIT_ROUNDS:-3}"

TMP=$(mktemp -d /tmp/lumabri-segment-split.XXXXXX)
PIDS=()
failed() {
    local status=$?
    echo "SEGMENT SPLIT TEST: FAIL" >&2
    for log in "$TMP"/*.log; do
        [[ -f "$log" ]] || continue
        echo "--- $(basename "$log")" >&2; tail -n 80 "$log" >&2 || true
    done
    return "$status"
}
cleanup() {
    kill -CONT "${PIDS[@]}" 2>/dev/null || true
    kill "${PIDS[@]}" 2>/dev/null || true
    wait "${PIDS[@]}" 2>/dev/null || true
    rm -rf "$TMP"
}
trap failed ERR
trap cleanup EXIT

[[ -x ./segment_node && -x ./segment_chat && -x ./tracker ]] ||
    { echo "SEGMENT SPLIT TEST: SKIP (build segment_node, segment_chat, tracker)"; exit 0; }
[[ -f "$MODEL_DIR/config.json" ]] ||
    { echo "SEGMENT SPLIT TEST: SKIP (no $MODEL_DIR)"; exit 0; }

LAYERS=$(python3 -c "import json;print(json.load(open('$MODEL_DIR/config.json'))['num_hidden_layers'])")
MTYPE=$(python3 -c "import json;print(json.load(open('$MODEL_DIR/config.json')).get('model_type',''))")
[[ "$LAYERS" -ge 2 ]] || { echo "SEGMENT SPLIT TEST: SKIP ($MODEL_DIR has $LAYERS layers)"; exit 0; }

wait_port() {
    for _ in $(seq 1 400); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then exec 3<&-; exec 3>&-; return 0; fi
        sleep .05
    done
    return 1
}

prompt=$(python3 - "$MODEL_DIR" <<'PY'
import json,sys,os
p=os.path.join(sys.argv[1],'ref.json')
print(','.join(map(str,json.load(open(p,encoding='utf-8'))['prompt_ids']))
      if os.path.exists(p) else '1,2,3,4')
PY
)
model_root=$(printf '%064x' 191)
tokenizer_root=$(printf '%064x' 192)

# A fresh tracker per configuration. Sharing one across phases leaves the
# previous phase's nodes advertised after they are killed, and the chat then
# dials a dead address instead of the topology under test.
TRACKER=0
PHASE=0
start_phase() {              # $1 = nodes, $2 = threads each, $3 = tag
    local n=$1 threads=$2 tag=$3 i begin end per base
    PHASE=$(( PHASE + 1 ))
    TRACKER=$(( PORT + 100 * PHASE ))
    base=$(( TRACKER + 10 ))
    LUMABRI_PEER_KEY="$TMP/$tag-tracker.key" \
    LUMABRI_KNOWN_HOSTS="$TMP/$tag-tracker.hosts" \
        ./tracker --port "$TRACKER" --peer-bindings "$TMP/$tag-bindings" \
        >"$TMP/$tag-tracker.log" 2>&1 & PIDS+=("$!")
    wait_port "$TRACKER"
    per=$(( (LAYERS + n - 1) / n ))
    for (( i = 0; i < n; i++ )); do
        begin=$(( i * per )); end=$(( begin + per ))
        (( end > LAYERS )) && end=$LAYERS
        (( begin >= end )) && continue
        LUMABRI_PEER_KEY="$TMP/$tag-$i.key" LUMABRI_KNOWN_HOSTS="$TMP/$tag-$i.hosts" \
        LUMABRI_VERIFY=0 OMP_NUM_THREADS="$threads" \
            ./segment_node --engine "$ENGINE_ID" --model-dir "$MODEL_DIR" \
            --model "$MODEL_NAME" --range "$begin:$end" \
            --port "$(( base + i ))" --tracker "127.0.0.1:$TRACKER" \
            --advertise "127.0.0.1:$(( base + i ))" --name "$tag-$i" \
            --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
            --context "$CONTEXT" --max-rows 16 --sessions 2 --threads "$threads" \
            >"$TMP/$tag-$i.log" 2>&1 &
        PIDS+=("$!")
        wait_port "$(( base + i ))"
    done
    sleep 3          # let the route generation settle before the first prompt
}

stop_phase() {
    local pid
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait "${PIDS[@]}" 2>/dev/null || true
    PIDS=()
    sleep 1
}

# One chat run: prints "<seconds> <token ids>"
run_chat() {                 # $1 = threads, $2 = log name
    local threads=$1 log=$2 t0 t1
    t0=$(date +%s.%N)
    OMP_NUM_THREADS="$threads" ./segment_chat --engine "$ENGINE_ID" \
        --model-dir "$MODEL_DIR" --model "$MODEL_NAME" \
        --tracker "127.0.0.1:$TRACKER" \
        --model-root "$model_root" --tokenizer-root "$tokenizer_root" \
        --prompt-ids "$prompt" --tokens "$TOKENS" --context "$CONTEXT" \
        --max-rows 16 --retry-first-run --json \
        >"$TMP/$log.json" 2>"$TMP/$log.log"
    t1=$(date +%s.%N)
    python3 - "$TMP/$log.json" "$t0" "$t1" <<'PY'
import json,sys
lines=[l for l in open(sys.argv[1],encoding='utf-8') if l.startswith('{')]
ids=json.loads(lines[-1])['token_ids']
print("%.3f %s" % (float(sys.argv[3])-float(sys.argv[2]), ','.join(map(str,ids))))
PY
}

best_of() {                  # $1 = threads, $2 = tag; prints "<best s> <ids>"
    local threads=$1 tag=$2 r out t ids best=""
    # One discarded run first. Without it the first phase measured is the one
    # that faults the checkpoint in from disk and every later phase reads it
    # from page cache — which made splitting look six times FASTER than not
    # splitting, an impossibility for sequential layers, and a measurement of
    # the running order rather than the topology.
    run_chat "$threads" "$tag-warm" >/dev/null
    for (( r = 0; r < ROUNDS; r++ )); do
        out=$(run_chat "$threads" "$tag-$r")
        t=${out%% *}; ids=${out#* }
        if [[ -z "$best" ]] || (( $(python3 -c "print(1 if $t < $best else 0)") )); then best=$t; fi
        echo "$ids" >>"$TMP/$tag.ids"
    done
    sort -u "$TMP/$tag.ids" >"$TMP/$tag.uniq"
    if [[ $(wc -l <"$TMP/$tag.uniq") -ne 1 ]]; then
        echo "SEGMENT SPLIT TEST: FAIL — $tag was not deterministic across $ROUNDS runs" >&2
        cat "$TMP/$tag.uniq" >&2
        exit 1
    fi
    echo "$best $(cat "$TMP/$tag.uniq")"
}

echo "model $MODEL_NAME ($MTYPE) · $LAYERS layers · $TOKENS tokens · best of $ROUNDS"
echo "  (one warm-up run per phase is discarded: the first read of a"
echo "   checkpoint is disk, every later one is page cache)"

# A) every layer on one node — the oracle, and the floor to compare against
start_phase 1 "$THREADS_TOTAL" whole
whole=$(best_of "$THREADS_TOTAL" whole)
whole_s=${whole%% *}; oracle=${whole#* }
echo "  A  1 node,  $THREADS_TOTAL threads   ${whole_s}s"
stop_phase

# B) split across two, SAME total threads: isolates the cost of the boundary
half=$(( THREADS_TOTAL / 2 )); (( half < 1 )) && half=1
start_phase 2 "$half" split
split=$(best_of "$THREADS_TOTAL" split)
split_s=${split%% *}; split_ids=${split#* }
echo "  B  2 nodes, $half+$half threads   ${split_s}s"

if [[ "$split_ids" != "$oracle" ]]; then
    echo "SEGMENT SPLIT TEST: FAIL — splitting the model changed the tokens" >&2
    echo "  one node : $oracle" >&2
    echo "  two nodes: $split_ids" >&2
    exit 1
fi

stop_phase

# C) split across two with all the cores each: what a person actually gets
start_phase 2 "$THREADS_TOTAL" full
full=$(best_of "$THREADS_TOTAL" full)
full_s=${full%% *}; full_ids=${full#* }
echo "  C  2 nodes, $THREADS_TOTAL+$THREADS_TOTAL threads   ${full_s}s"
[[ "$full_ids" == "$oracle" ]] || {
    echo "SEGMENT SPLIT TEST: FAIL — the tokens changed with all cores" >&2; exit 1; }

# Relay must never have been used: these nodes advertise direct addresses.
if grep -qi "relay" "$TMP"/split-*.log "$TMP"/whole-*.log 2>/dev/null; then
    echo "SEGMENT SPLIT TEST: FAIL — a run used the tracker relay; the numbers" \
         "would measure the tunnel, not the split" >&2
    exit 1
fi

# Timings are only reported when the machine could plausibly produce them.
#
# The invariance claim above is machine-independent: identical tokens are
# identical on a loaded laptop too. The cost claim is not. On a box whose run
# queue is longer than its cores, every phase is competing with something
# else, and the numbers say more about what else was running than about the
# topology — on this laptop at load 17 the split came out five times FASTER
# than the whole model, which is impossible for sequential layers.
#
# So the rule is: print the comparison only when the load average is below
# the core count, and otherwise say plainly that this machine cannot answer.
python3 - "$whole_s" "$split_s" "$full_s" <<'PY'
import os, sys
w, s, f = (float(x) for x in sys.argv[1:4])
try:
    load = os.getloadavg()[0]
    cores = os.cpu_count() or 1
except OSError:
    load, cores = 0.0, 1
if load > cores * 0.7:
    print("  timings withheld: load %.1f on %d cores. The split is proved"
          % (load, cores))
    print("  CORRECT here, not fast — run this on a quiet machine for cost.")
    sys.exit(0)
print("  boundary cost, equal threads: %+.1f%%  (%.3fs -> %.3fs)"
      % ((s - w) / w * 100.0, w, s))
print("  two nodes, all cores each:    %+.1f%%  (%.3fs -> %.3fs)"
      % ((f - w) / w * 100.0, w, f))
if s < w * 0.9:
    print("  NOTE: splitting came out faster at equal threads. Layers are")
    print("  sequential, so that cannot be a property of the split — suspect")
    print("  page cache, an unequal thread count, or contention.")
PY

echo "SEGMENT SPLIT TEST: PASS (tokens identical across 1 and 2 nodes, relay unused)"
