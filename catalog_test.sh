#!/usr/bin/env bash
# The catalogue's job is to be believed, so its failure mode is not "wrong
# number" but "confident number". Three things it must never do:
#
#   say a model runs when the machine cannot hold it;
#   say nothing is missing when something is;
#   print a speed for a plan nobody has measured.
set -euo pipefail
cd "$(dirname "$0")"
[[ -x ./lumabri ]] || { echo "CATALOG TEST: SKIP (build lumabri)"; exit 0; }

T=$(mktemp -d /tmp/lumabri-catalog.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "CATALOG TEST: FAIL — $*" >&2; exit 1; }

# A model far larger than any laptop, and one that fits anywhere.
mkdir -p "$T/models/huge" "$T/models/tiny"
cat >"$T/models/huge/config.json" <<'EOF'
{"model_type":"deepseek_v4","num_hidden_layers":43,"hidden_size":4096,
 "intermediate_size":11264,"moe_intermediate_size":1408,"n_routed_experts":256,
 "num_experts_per_tok":6,"num_attention_heads":32,"num_key_value_heads":8,
 "vocab_size":129280}
EOF
cat >"$T/models/tiny/config.json" <<'EOF'
{"model_type":"olmoe","num_hidden_layers":4,"hidden_size":64,
 "intermediate_size":128,"num_experts":8,"num_experts_per_tok":2,
 "num_attention_heads":4,"num_key_value_heads":4,"vocab_size":256}
EOF

out=$(./lumabri models --models-dir "$T/models" 2>&1) || fail "the catalogue exited non-zero"
echo "$out" | sed 's/^/    /'

huge=$(echo "$out" | grep -E '^\s*[x+!]\s+huge' || true)
tiny=$(echo "$out" | grep -E '^\s*[x+!]\s+tiny' || true)
[[ -n "$huge" && -n "$tiny" ]] || fail "not every checkpoint appeared in the catalogue"

# A ~150 GB model on one machine is not runnable, and saying otherwise is the
# whole failure this screen exists to prevent.
echo "$huge" | grep -q "not runnable" ||
    fail "a 150 GB model was reported as runnable on this machine: $huge"
# ...and "not runnable" alone is useless: it has to say how far short, in a
# unit somebody can act on.
echo "$huge" | grep -qE "[0-9]+ GB short" ||
    fail "the shortfall was not stated in gigabytes: $huge"
echo "$huge" | grep -qE "more machine" ||
    fail "the shortfall was not stated in machines, which is what a person buys: $huge"

echo "$tiny" | grep -q "resident" ||
    fail "a four-layer toy model was not reported as resident: $tiny"

# No speed, anywhere, ever, until something is measured.
echo "$out" | grep -q "not calibrated" ||
    fail "the speed column did not say the plan is uncalibrated"
if echo "$out" | grep -qE "[0-9]+([.,][0-9]+)? *tok/s"; then
    fail "a tok/s figure appeared for a plan nobody has measured"
fi

# A directory with no checkpoints says so instead of printing an empty table.
mkdir -p "$T/empty"
./lumabri models --models-dir "$T/empty" 2>&1 | grep -qi "no checkpoints" ||
    fail "an empty models directory did not say it was empty"

echo "CATALOG TEST: PASS (states honest, shortfall in machines, no invented speed)"
