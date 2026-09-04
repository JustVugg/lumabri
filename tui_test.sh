#!/usr/bin/env bash
# A screen can lie in ways a function cannot: by truncating the one sentence
# a person acts on, by showing a number for a plan nobody measured, or by
# leaving the terminal in raw mode when it exits.
#
# --snapshot renders one frame to stdout with no cursor moves, so all of that
# is testable without a pty. --keys feeds a key sequence, so navigation and
# the detail view are testable too.
set -euo pipefail
cd "$(dirname "$0")"
[[ -x ./lumabri ]] || { echo "TUI TEST: SKIP (build lumabri)"; exit 0; }

T=$(mktemp -d /tmp/lumabri-tui.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "TUI TEST: FAIL — $*" >&2; exit 1; }

mkdir -p "$T/models/huge" "$T/models/small"
cat >"$T/models/huge/config.json" <<'EOF'
{"model_type":"deepseek_v4","num_hidden_layers":43,"hidden_size":4096,
 "intermediate_size":11264,"moe_intermediate_size":1408,"n_routed_experts":256,
 "num_experts_per_tok":6,"num_attention_heads":32,"num_key_value_heads":8,
 "vocab_size":129280}
EOF
cat >"$T/models/small/config.json" <<'EOF'
{"model_type":"olmoe","num_hidden_layers":4,"hidden_size":64,
 "intermediate_size":128,"num_experts":8,"num_experts_per_tok":2,
 "num_attention_heads":4,"num_key_value_heads":4,"vocab_size":256}
EOF

snap() { ./lumabri models --models-dir "$T/models" --snapshot "$@" 2>&1; }

# ---- the catalogue -------------------------------------------------------
list=$(snap)
echo "$list" | sed 's/^/    /'

grep -q "huge" <<<"$list"  || fail "the catalogue did not list every model"
grep -q "small" <<<"$list" || fail "the catalogue did not list every model"

# The actionable sentence must arrive whole. Truncating "98 GB short (~6 more
# machines)" to "98 GB short (~6 more ma" turns the only line a person can act
# on into noise, and it is the exact failure a fixed-width column produces.
if grep -qE "more machine" <<<"$list"; then
    grep -qE "more machines?\)" <<<"$list" ||
        fail "the shortfall was cut mid-sentence:
$(grep 'more ma' <<<"$list")"
else
    fail "a model that does not fit did not say how many machines are missing"
fi
grep -qE "[0-9]+ GB short" <<<"$list" || fail "no shortfall in gigabytes"
grep -q "not calibrated" <<<"$list"   || fail "the speed column claimed something"
grep -qE "[0-9]+([.,][0-9]+)? *tok/s" <<<"$list" &&
    fail "a tok/s figure appeared with nothing measured"

# No cursor positioning in a snapshot: it is meant to be read and diffed.
grep -qP '\x1b\[[0-9]+;[0-9]+H' <<<"$list" &&
    fail "the snapshot emitted cursor moves; it is unreadable in a pipe or a test"
# ...and no colour, for the same reason.
grep -qP '\x1b\[[0-9;]*m' <<<"$list" &&
    fail "the snapshot emitted colour escapes"

# ---- navigation and the detail ------------------------------------------
detail=$(snap --keys $'\r')
grep -q "WHAT IT NEEDS" <<<"$detail" || fail "⏎ did not open the detail view"
grep -q "working set" <<<"$detail" ||
    fail "the detail did not separate the working set from the resident cost;
  that gap IS disk mode, and hiding it removes a whole state of the product"
grep -qE "edge, embedding and head" <<<"$detail" ||
    fail "the detail did not attribute Edge to anybody"
grep -qE "state for [0-9]+ session" <<<"$detail" ||
    fail "the detail did not show what sessions and context cost"
grep -q "not calibrated" <<<"$detail" || fail "the detail claimed a speed"

# Moving the selection changes which model the detail opens.
first=$(snap --keys $'\r' | grep -E "^[a-z]" | head -1)
second=$(snap --keys $'j\r' | grep -E "^[a-z]" | head -1)
[[ "$first" != "$second" ]] ||
    fail "↓ then ⏎ opened the same model as ⏎ alone: the selection does not move"

# ---- the other tab -------------------------------------------------------
nodes=$(snap --keys $'\t')
grep -q "COMPUTER" <<<"$nodes" || fail "tab did not reach the computers view"
grep -qE "CPU only|can use its GPU" <<<"$nodes" ||
    fail "the computers view did not say whether the ENGINE can use a GPU;
  a card the engine cannot drive is not a GPU machine, and the plan depends
  on the difference"

# ---- a pipe gets the plain listing, not a screen -------------------------
piped=$(./lumabri models --models-dir "$T/models" | cat)
grep -q "MODEL" <<<"$piped" || fail "piping produced no listing"
grep -qP '\x1b\[\?1049h' <<<"$piped" &&
    fail "piping the output still switched to the alternate screen"

# ---- the terminal comes back ---------------------------------------------
# The one bug a TUI can leave behind for the shell that outlives it. Drive a
# real pty, quit, and check the settings are the ones we started with.
if python3 ./tui_pty_check.py "$T/models"; then :
else fail "the terminal was left in raw mode after quitting"; fi

echo "TUI TEST: PASS (nothing truncated, nothing claimed, terminal restored)"

