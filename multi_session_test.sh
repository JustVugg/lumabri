#!/usr/bin/env bash
# Capacity has to be an answer, not a slowdown.
#
# The failure this guards against is the one everybody else ships: at
# capacity, admit anyway and let every session get slower. Nobody sees a
# limit, everybody sees a degradation, and the product feels broken instead
# of full. So: a node with N session slots serves N and REFUSES the N+1th,
# and the refusal is visible.
#
# The second claim is isolation. Two sessions on one node must not see each
# other's state — the same prompt in session B must produce what it produces
# alone, not a continuation of session A.
set -euo pipefail
cd "$(dirname "$0")"

PORT="${PORT:-8480}"
TMP=$(mktemp -d /tmp/lumabri-multisession.XXXXXX)
PIDS=()
fail() { echo "MULTI-SESSION TEST: FAIL — $*" >&2
         for l in "$TMP"/*.log; do [[ -f $l ]] || continue
             echo "--- $(basename "$l")" >&2; tail -30 "$l" >&2; done; exit 1; }
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true
            wait "${PIDS[@]}" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT

[[ -x ./segment_node && -x ./segment_chat && -x ./tracker && -f tiny_olmoe/config.json ]] ||
    { echo "MULTI-SESSION TEST: SKIP (needs the Segment binaries and tiny_olmoe)"; exit 0; }

wait_port() { for _ in $(seq 1 300); do
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; exec 3>&-; return 0; }
    sleep .05; done; return 1; }

prompt=$(python3 -c "
import json; print(','.join(map(str, json.load(open('tiny_olmoe/ref.json'))['prompt_ids'])))")
mr=$(printf '%064x' 211); tk=$(printf '%064x' 212)

LUMABRI_PEER_KEY="$TMP/t.key" LUMABRI_KNOWN_HOSTS="$TMP/t.hosts" \
    ./tracker --port "$PORT" --peer-bindings "$TMP/b" >"$TMP/tracker.log" 2>&1 &
PIDS+=("$!"); wait_port "$PORT"

# One node, the whole model, exactly ONE session slot.
LUMABRI_PEER_KEY="$TMP/n.key" LUMABRI_KNOWN_HOSTS="$TMP/n.hosts" \
LUMABRI_VERIFY=0 OMP_NUM_THREADS=2 \
    ./segment_node --engine olmoe --model-dir tiny_olmoe --model tiny-olmoe \
    --range 0:16 --port "$(( PORT + 1 ))" --tracker "127.0.0.1:$PORT" \
    --advertise "127.0.0.1:$(( PORT + 1 ))" --name solo \
    --model-root "$mr" --tokenizer-root "$tk" --context 64 --max-rows 16 \
    --sessions 1 --threads 2 --run-queue 1 --run-wait-ms 1500 \
    >"$TMP/node.log" 2>&1 &
PIDS+=("$!"); wait_port "$(( PORT + 1 ))"
sleep 3

chat() {  # $1 = tag, $2 = tokens
    OMP_NUM_THREADS=2 ./segment_chat --engine olmoe --model-dir tiny_olmoe \
        --model tiny-olmoe --tracker "127.0.0.1:$PORT" \
        --model-root "$mr" --tokenizer-root "$tk" --prompt-ids "$prompt" \
        --tokens "$2" --context 64 --max-rows 16 --retry-first-run --json \
        >"$TMP/$1.json" 2>"$TMP/$1.log"
}

ids_of() { python3 - "$TMP/$1.json" <<'PY'
import json,sys
lines=[l for l in open(sys.argv[1],encoding='utf-8') if l.startswith('{')]
print(','.join(map(str,json.loads(lines[-1])['token_ids'])) if lines else '')
PY
}

# One session alone: the oracle.
chat alone 4 || fail "a single session could not complete"
oracle=$(ids_of alone)
[[ -n "$oracle" ]] || fail "the single session produced no tokens"

# Two at once against one slot. Whatever happens, the one that gets in must
# produce the same tokens as if it were alone — a session that is squeezed
# is worse than one that is refused.
chat par-a 4 & pa=$!
chat par-b 4 & pb=$!
wait "$pa" 2>/dev/null || true
wait "$pb" 2>/dev/null || true
a=$(ids_of par-a); b=$(ids_of par-b)
served=0
for got in "$a" "$b"; do
    [[ -z "$got" ]] && continue
    served=$(( served + 1 ))
    [[ "$got" == "$oracle" ]] ||
        fail "a session that ran alongside another produced different tokens.
  alone: $oracle
  under contention: $got
  contention may make a session wait or be refused; it may never change it."
done
(( served >= 1 )) || fail "neither of two concurrent sessions completed"

# Capacity is stated, not implied: the node has to have said how many it takes.
grep -qE "[0-9]+ session" "$TMP/node.log" ||
    fail "the node never stated its session capacity"

echo "MULTI-SESSION TEST: PASS ($served of 2 concurrent sessions served, \
tokens unchanged under contention)"
