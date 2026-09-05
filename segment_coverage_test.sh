#!/usr/bin/env bash
# A partial model must refuse, not answer.
#
# This is the one failure that cannot be allowed to be quiet. A chain
# missing layers 20 to 30 will still produce tokens — they are simply the
# wrong tokens, from a model that is not the one anybody asked for. Nothing
# about the output says so. So the refusal has to happen before generation,
# and it has to be loud.
#
# Two claims: an incomplete chain refuses and says why; the same nodes plus
# the missing range then work, so the refusal was about coverage and not
# about something else being broken.
set -euo pipefail
cd "$(dirname "$0")"

PORT="${PORT:-8520}"
TMP=$(mktemp -d /tmp/lumabri-coverage.XXXXXX)
PIDS=()
fail() { echo "SEGMENT COVERAGE TEST: FAIL — $*" >&2
         for l in "$TMP"/*.log; do [[ -f $l ]] || continue
             echo "--- $(basename "$l")" >&2; tail -25 "$l" >&2; done; exit 1; }
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true
            wait "${PIDS[@]}" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT

[[ -x ./segment_node && -x ./segment_chat && -x ./tracker && -f tiny_olmoe/config.json ]] ||
    { echo "SEGMENT COVERAGE TEST: SKIP (needs the Segment binaries and tiny_olmoe)"; exit 0; }

wait_port() { for _ in $(seq 1 300); do
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; exec 3>&-; return 0; }
    sleep .05; done; return 1; }

prompt=$(python3 -c "
import json; print(','.join(map(str, json.load(open('tiny_olmoe/ref.json'))['prompt_ids'])))")
mr=$(printf '%064x' 221); tk=$(printf '%064x' 222)

LUMABRI_PEER_KEY="$TMP/t.key" LUMABRI_KNOWN_HOSTS="$TMP/t.hosts" \
    ./tracker --port "$PORT" --peer-bindings "$TMP/b" >"$TMP/tracker.log" 2>&1 &
PIDS+=("$!"); wait_port "$PORT"

start_range() {   # $1 = begin, $2 = end, $3 = index
    LUMABRI_PEER_KEY="$TMP/n$3.key" LUMABRI_KNOWN_HOSTS="$TMP/n$3.hosts" \
    LUMABRI_VERIFY=0 OMP_NUM_THREADS=2 \
        ./segment_node --engine olmoe --model-dir tiny_olmoe --model tiny-olmoe \
        --range "$1:$2" --port "$(( PORT + 10 + $3 ))" --tracker "127.0.0.1:$PORT" \
        --advertise "127.0.0.1:$(( PORT + 10 + $3 ))" --name "cov-$3" \
        --model-root "$mr" --tokenizer-root "$tk" --context 64 --max-rows 16 \
        --sessions 1 --threads 2 >"$TMP/n$3.log" 2>&1 &
    PIDS+=("$!"); wait_port "$(( PORT + 10 + $3 ))"
}

chat() {  # $1 = tag; returns the chat's exit status
    OMP_NUM_THREADS=2 timeout 120 ./segment_chat --engine olmoe \
        --model-dir tiny_olmoe --model tiny-olmoe --tracker "127.0.0.1:$PORT" \
        --model-root "$mr" --tokenizer-root "$tk" --prompt-ids "$prompt" \
        --tokens 3 --context 64 --max-rows 16 --json \
        >"$TMP/$1.json" 2>"$TMP/$1.log"
}

# Layers 0:6 and 10:16 — the middle of the model is simply absent.
start_range 0 6 0
start_range 10 16 1
sleep 3

# The chatter has its own copy of the weights, so a local fallback is a
# legitimate outcome here. What is NOT legitimate is a chain that runs
# through the two partial nodes and calls the result an answer.
if chat gap; then
    grep -qE "running experts locally|local" "$TMP/gap.log" ||
        fail "an incomplete chain produced an answer without falling back locally.
  Layers 6 to 10 exist nowhere, so those tokens came from a model that is
  not the one that was asked for, and nothing in the output says so."
else
    grep -qiE "coverage|chain|cannot" "$TMP/gap.log" ||
        fail "an incomplete chain failed without saying it was about coverage:
$(tail -5 "$TMP/gap.log")"
fi

# Fill the gap: the same nodes plus 6:10 must now form a chain.
start_range 6 10 2
sleep 4
chat whole || fail "a complete chain still refused to run"
ids=$(python3 - "$TMP/whole.json" <<'PY'
import json,sys
lines=[l for l in open(sys.argv[1],encoding='utf-8') if l.startswith('{')]
print(','.join(map(str,json.loads(lines[-1])['token_ids'])) if lines else '')
PY
)
[[ -n "$ids" ]] || fail "a complete chain produced no tokens"
grep -qE "cov-0\[0:6\].*cov-2\[6:10\].*cov-1\[10:16\]" \
    "$TMP/whole.json" "$TMP/whole.log" ||
    fail "the complete-chain run produced tokens without proving that the exact 0:16 Segment route served them"

echo "SEGMENT COVERAGE TEST: PASS (a gap in the chain never silently answers)"
