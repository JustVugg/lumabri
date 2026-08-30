#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d /tmp/lumabri-production-gate.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"
printf '#!/bin/sh\nexit 0\n' >"$tmp/bin/make"
chmod +x "$tmp/bin/make"
PATH="$tmp/bin:$PATH" bash ./production_gate.sh \
    --engine "$tmp/no-segment-colibri" --output "$tmp/result" \
    >"$tmp/gate.log"
python3 - "$tmp/result/production-gate.json" <<'PY'
import json, sys
result = json.load(open(sys.argv[1], encoding="utf-8"))
assert result["schema"] == 1 and result["ok"] is True
names = [step["name"] for step in result["steps"]]
assert names == ["patches", "warnings", "unit-integration", "sanitizers",
                 "segment-hybrid-skipped", "multihost-skipped"]
assert all(step["ok"] for step in result["steps"])
PY
echo "PRODUCTION GATE TEST: PASS (step manifest and explicit skips)"
