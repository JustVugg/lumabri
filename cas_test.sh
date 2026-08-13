#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

make -s tracker maintainer liblumabri.so test_shim
T=$(mktemp -d /tmp/lumabri-cas.XXXXXX)
export LUMABRI_PEER_BINDINGS="$T/peer-bindings"
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
mkdir -p "$T/src" "$T/cache-a" "$T/cache-b" "$T/cas"
head -c $((2 * 1024 * 1024 + 777)) /dev/urandom >"$T/src/weights.bin"
printf '{"model":"cas-test"}\n' >"$T/src/config.json"

./tracker --port 7562 >"$T/tracker.log" 2>&1 & TRACKER_PID=$!; PIDS+=($!)
./maintainer --root "$T/src" --port 7563 --tracker 127.0.0.1:7562 \
    --name cas-origin --model-name cas-test >"$T/node.log" 2>&1 & NODE_PID=$!; PIDS+=($!)
for _ in $(seq 1 200); do
    (exec 3<>/dev/tcp/127.0.0.1/7563) 2>/dev/null && { exec 3<&-; break; }
    sleep .1
done
sleep .3

COMMON=(LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_TRACKER=127.0.0.1:7562
        LUMABRI_MODEL=cas-test LUMABRI_BLOCK_MIB=1 LUMABRI_CAS="$T/cas")
env "${COMMON[@]}" LUMABRI_VROOT="$T/vroot-a" LUMABRI_CACHE="$T/cache-a" \
    ./test_shim "$T/vroot-a" "$T/src" >/dev/null
find "$T/cas" -type f -name '[0-9a-f]*' -print -quit | grep -q . || {
    echo "CAS was not populated"; exit 1;
}

# A fresh sparse mirror must now work with the byte server gone. The tracker
# remains only to provide the signed/hash inventory; no model byte may cross
# the network on this second run.
kill "$NODE_PID" 2>/dev/null || true
wait "$NODE_PID" 2>/dev/null || true
env "${COMMON[@]}" LUMABRI_STATS=1 LUMABRI_VROOT="$T/vroot-b" \
    LUMABRI_CACHE="$T/cache-b" ./test_shim "$T/vroot-b" "$T/src" \
    >/dev/null 2>"$T/cas.err"
echo "LOCAL CAS: PASS (fresh mirror rebuilt with origin offline)"
