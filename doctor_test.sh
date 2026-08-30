#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

tmp=$(mktemp -d /tmp/lumabri-doctor.XXXXXX)
tracker_pid=""
cleanup() {
    if [[ -n "$tracker_pid" ]]; then
        kill "$tracker_pid" 2>/dev/null || true
        wait "$tracker_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
export HOME="$tmp"

./lumabri doctor --json >"$tmp/ready.json"
python3 - "$tmp/ready.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["schema"] == 1 and data["ok"] is True
names = {item["name"] for item in data["checks"]}
assert {"machine-profile", "state-directory", "binary-tracker",
        "binary-maintainer", "binary-swarm_probe", "library-liblumabri"} <= names
PY

if ./lumabri doctor --json --model "$tmp/not-a-model" >"$tmp/model.json"; then
    echo "doctor accepted a missing model" >&2
    exit 1
fi
python3 - "$tmp/model.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
checks = {item["name"]: item for item in data["checks"]}
assert data["ok"] is False and checks["model-config"]["ok"] is False
PY

./tracker --port 7990 --peer-bindings "$tmp/bindings" >"$tmp/tracker.log" 2>&1 &
tracker_pid=$!
for _ in $(seq 1 100); do
    if (exec 3<>/dev/tcp/127.0.0.1/7990) 2>/dev/null; then
        exec 3<&-; exec 3>&-; break
    fi
    sleep .02
done
if ./lumabri doctor --json --serve-port 7990 >"$tmp/port.json"; then
    echo "doctor accepted an occupied serving topology" >&2
    exit 1
fi
python3 - "$tmp/port.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
checks = {item["name"]: item for item in data["checks"]}
assert checks["serve-ports"]["ok"] is False
assert "7990" in checks["serve-ports"]["detail"]
PY

echo "LUMABRI DOCTOR TEST: PASS (JSON contract, model, occupied topology)"
