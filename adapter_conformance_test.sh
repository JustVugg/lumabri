#!/usr/bin/env bash
# "Supported" has to mean "proved", not "compiles".
#
# Every adapter Colibri exposes gets the same battery, and an adapter that
# has not passed it is reported as experimental rather than supported. The
# list of adapters comes from Colibri's headers, so a new one shows up here
# as untested the moment it appears — the same rule model_family_test.sh
# enforces for the registry.
#
# A family with no fixture checkpoint on this machine is SKIPPED, and skipped
# is not passed: the summary says so, and the exit status stays zero only
# because a laptop cannot be expected to hold a 167 GB checkpoint. The real
# matrix runs where the checkpoints live. Set LUMABRI_REQUIRE_ALL_ADAPTERS=1
# there: then a missing fixture is a failure, so green really means that every
# registered adapter was exercised rather than merely enumerated.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../colibri/c}"
[[ -f "$ENGINE/segment_adapters.h" ]] ||
    { echo "ADAPTER CONFORMANCE: SKIP (no $ENGINE)"; exit 0; }

adapters=$(grep -oE "coli_[a-z0-9_]+_segment_adapter_register" \
           "$ENGINE/segment_adapters.h" |
           sed -E 's/^coli_(.*)_segment_adapter_register$/\1/' | sort -u)

# Where a fixture for each family lives, when one does. LUMABRI_FIXTURE_<id>
# overrides, so an operator with the real checkpoints can run the whole
# matrix without editing this file.
fixture_for() {
    local id=$1 var
    var="LUMABRI_FIXTURE_${id^^}"
    if [[ -n "${!var:-}" ]]; then echo "${!var}"; return; fi
    case "$id" in
        olmoe) [[ -d tiny_olmoe ]] && echo tiny_olmoe ;;
        *)     ;;
    esac
}

pass=0 skip=0 fail=0
declare -a untested=()
for id in $adapters; do
    dir=$(fixture_for "$id")
    if [[ -z "$dir" || ! -f "$dir/config.json" ]]; then
        printf '  %-14s experimental — no checkpoint on this machine\n' "$id"
        untested+=("$id"); skip=$(( skip + 1 ))
        continue
    fi
    name=$(python3 -c "import json;print(json.load(open('$dir/config.json')).get('model_type','$id'))")
    if SPLIT_MODEL_DIR="$dir" SPLIT_ENGINE="$id" SPLIT_MODEL="conf-$id" \
       PORT=$(( 8200 + RANDOM % 200 )) bash ./segment_split_test.sh >"/tmp/conf-$id.log" 2>&1; then
        printf '  %-14s supported (%s)\n' "$id" "$name"
        pass=$(( pass + 1 ))
    else
        printf '  %-14s FAILED — see /tmp/conf-%s.log\n' "$id" "$id"
        tail -n 20 "/tmp/conf-$id.log" >&2
        fail=$(( fail + 1 ))
    fi
done

echo "ADAPTER CONFORMANCE: $pass supported, $skip experimental, $fail failed"
if (( fail > 0 )); then exit 1; fi
if (( pass == 0 )); then
    echo "  nothing was actually exercised: this run proves nothing" >&2
fi
if [[ "${LUMABRI_REQUIRE_ALL_ADAPTERS:-0}" == 1 ]] && (( skip > 0 )); then
    printf '  strict run requires all adapters; missing:' >&2
    printf ' %s' "${untested[@]}" >&2
    printf '\n' >&2
    exit 1
fi
exit 0
