#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

tmp=$(mktemp -d /tmp/lumabri-serve-failfast.XXXXXX)
listener=
cleanup() {
    [[ -z "$listener" ]] || kill "$listener" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

mkdir -p "$tmp/model"
printf '{"model_type":"olmoe","num_hidden_layers":1}\n' >"$tmp/model/config.json"

# Occupy the storage port, not the tracker port. The old implementation had
# already spawned the tracker when the maintainer discovered EADDRINUSE.
python3 - 7951 <<'PY' &
import socket,sys,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("127.0.0.1",int(sys.argv[1]))); s.listen(); time.sleep(30)
PY
listener=$!
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/7951) 2>/dev/null && { exec 3<&-; exec 3>&-; break; }
    sleep .02
done

set +e
./lumabri serve --model "$tmp/model" --port 7950 --advertise 127.0.0.1 \
    --no-exec >"$tmp/out" 2>&1
status=$?
set -e
if (( status == 0 )) || ! grep -q 'port 7951 is already in use; nothing was started' "$tmp/out"; then
    cat "$tmp/out"
    echo "serve did not fail atomically on an occupied child port" >&2
    exit 1
fi
if (exec 3<>/dev/tcp/127.0.0.1/7950) 2>/dev/null; then
    exec 3<&-; exec 3>&-
    echo "tracker was left running after preflight failure" >&2
    exit 1
fi
echo "SERVE FAIL-FAST TEST: PASS (occupied topology starts no children)"
