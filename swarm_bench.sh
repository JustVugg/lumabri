#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 SPEC.json OUTPUT.json [swarm_bench.py options...]" >&2
    exit 2
fi
spec=$1
output=$2
shift 2
exec python3 "$(dirname "$0")/swarm_bench.py" --spec "$spec" --output "$output" "$@"
