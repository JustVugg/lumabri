#!/usr/bin/env bash
# Reproducible release gate. It never installs software, opens firewalls,
# starts remote donors or merges code; those hosts are explicit in the spec.
set -u -o pipefail
cd "$(dirname "$0")"

engine=${ENGINE:-../colibri/c}
output="artifacts/production-$(date -u +%Y%m%dT%H%M%SZ)"
multi_host=""
require_multi=0
soak_seconds=0

while (($#)); do
    case "$1" in
        --engine) engine=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        --multi-host) multi_host=$2; shift 2 ;;
        --require-multihost) require_multi=1; shift ;;
        --soak-seconds) soak_seconds=$2; shift 2 ;;
        *) echo "usage: $0 [--engine DIR] [--output DIR] [--multi-host SPEC] [--require-multihost] [--soak-seconds N]" >&2; exit 2 ;;
    esac
done
if [[ ! "$soak_seconds" =~ ^[0-9]+$ ]]; then
    echo "--soak-seconds must be a non-negative integer" >&2
    exit 2
fi
if ((require_multi)) && [[ -z "$multi_host" ]]; then
    echo "--require-multihost needs --multi-host SPEC" >&2
    exit 2
fi
if ((soak_seconds > 0)) && [[ -z "$multi_host" ]]; then
    echo "--soak-seconds needs --multi-host SPEC" >&2
    exit 2
fi

mkdir -p "$output/logs"
steps="$output/steps.tsv"
: >"$steps"
failed=0

run_step() {
    local name=$1; shift
    local log="$output/logs/$name.log"
    local before after status
    before=$(date +%s)
    echo "==> $name"
    "$@" >"$log" 2>&1
    status=$?
    after=$(date +%s)
    printf '%s\t%s\t%s\t%s\n' "$name" "$status" "$((after-before))" "$log" >>"$steps"
    if ((status)); then
        echo "FAILED: $name (see $log)" >&2
        tail -80 "$log" >&2
        failed=1
        return 1
    fi
    echo "PASS: $name ($((after-before))s)"
}

finish() {
    python3 - "$steps" "$output/production-gate.json" "$failed" "$engine" "$multi_host" <<'PY'
import json, pathlib, sys, time
steps_path, output_path, failed, engine, spec = sys.argv[1:]
steps = []
for line in pathlib.Path(steps_path).read_text(encoding="utf-8").splitlines():
    name, status, seconds, log = line.split("\t")
    steps.append({"name": name, "ok": status == "0",
                  "seconds": int(seconds), "log": log})
result = {"schema": 1, "ok": failed == "0", "engine": engine,
          "multi_host_spec": spec or None, "steps": steps,
          "finished_at_unix": int(time.time())}
pathlib.Path(output_path).write_text(json.dumps(result, indent=2, sort_keys=True)+"\n",
                                     encoding="utf-8")
print(json.dumps(result, sort_keys=True))
PY
}
trap finish EXIT

run_step patches make patches-check "ENGINE=$engine" || exit 1
run_step warnings make check-warnings "ENGINE=$engine" || exit 1
run_step unit-integration make test "ENGINE=$engine" || exit 1
run_step sanitizers make test-sanitize || exit 1

if [[ -r "$engine/segment_runtime.h" && -r "$engine/edge_runtime.h" ]]; then
    run_step segment-hybrid make test-segment-hybrid "ENGINE=$engine" || exit 1
else
    printf '%s\t0\t0\t%s\n' segment-hybrid-skipped "$output/logs/segment-hybrid-skipped.log" >>"$steps"
    echo "Colibri Segment ABI absent: legacy build verified, Segment gate skipped" >"$output/logs/segment-hybrid-skipped.log"
fi

if [[ -n "$multi_host" ]]; then
    run_step bench-local python3 ./swarm_bench.py --spec "$multi_host" \
        --output "$output/baseline-local.json" --mode baseline-local || exit 1
    run_step bench-single python3 ./swarm_bench.py --spec "$multi_host" \
        --output "$output/baseline-single.json" --mode baseline-single || exit 1
    run_step bench-swarm python3 ./swarm_bench.py --spec "$multi_host" \
        --output "$output/swarm.json" --mode swarm || exit 1
    run_step performance-decision python3 ./production_check.py \
        --local "$output/baseline-local.json" \
        --single "$output/baseline-single.json" --swarm "$output/swarm.json" \
        --output "$output/performance-decision.json" || exit 1
    if ((soak_seconds > 0)); then
        run_step soak python3 ./swarm_soak.py --spec "$multi_host" \
            --seconds "$soak_seconds" --output "$output/soak.json" || exit 1
    fi
else
    printf '%s\t0\t0\t%s\n' multihost-skipped "$output/logs/multihost-skipped.log" >>"$steps"
    echo "No multi-host spec supplied; no A/B/C performance claim was made." >"$output/logs/multihost-skipped.log"
fi

echo "Production gate complete: $output/production-gate.json"
