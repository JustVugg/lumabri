#!/usr/bin/env bash
# A warm .lmap is only a presence hint. Signed truth must survive restart and
# every process must verify a present block before returning its bytes.
set -euo pipefail
cd "$(dirname "$0")"

make -s all
T=$(mktemp -d /tmp/lumabri-hot-cache.XXXXXX)
export LUMABRI_PEER_BINDINGS="$T/peer-bindings"
PIDS=()
cleanup() {
    if [ "${#PIDS[@]}" -gt 0 ]; then
        kill "${PIDS[@]}" 2>/dev/null || true
        wait "${PIDS[@]}" 2>/dev/null || true
    fi
    rm -rf "$T"
}
trap cleanup EXIT

wait_port() {
    for _ in $(seq 1 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; exec 3>&-; return 0; }
        sleep 0.1
    done
    return 1
}

stop_swarm() {
    if [ "${#PIDS[@]}" -gt 0 ]; then
        kill "${PIDS[@]}" 2>/dev/null || true
        wait "${PIDS[@]}" 2>/dev/null || true
        PIDS=()
    fi
}

start_swarm() {
    ./tracker --port 7590 --pubkey "$T/swarm.pub" >>"$T/tracker.log" 2>&1 & PIDS+=($!)
    wait_port 7590
    ./maintainer --root "$T/src" --port 7591 --tracker 127.0.0.1:7590 \
        --name origin --model-name fx --key "$T/swarm.key" >>"$T/origin.log" 2>&1 & PIDS+=($!)
    wait_port 7591
    sleep 1
}

run_shim() {
    local pubkey=${PUBKEY-$PUB}
    env LD_PRELOAD="$PWD/liblumabri.so" \
        LUMABRI_VROOT="$T/v" LUMABRI_CACHE="$T/cache" \
        LUMABRI_MODEL=fx LUMABRI_BLOCK_MIB="${BLOCK_MIB:-1}" \
        LUMABRI_PUBKEY="$pubkey" \
        LUMABRI_CAS="${CAS_DIR:-$T/cache/cas}" \
        ${TRACKER_ENV:-} ./test_shim "$T/v" "$T/src"
}

mkdir -p "$T/src"
python3 - "$T/src/w.bin" <<'PY'
import sys
p = sys.argv[1]
with open(p, "wb") as f:
    f.write(bytes(range(256)) * (12288 + 1))
PY
printf '{"model_type":"fixture"}\n' > "$T/src/config.json"
./lumabri key --out "$T/swarm" >/dev/null
./lumabri key --out "$T/other" >/dev/null
PUB=$(cat "$T/swarm.pub")

start_swarm

echo "· signed cold cache stores authenticated truth"
TRACKER_ENV=LUMABRI_TRACKER=127.0.0.1:7590 run_shim >"$T/cold.out" 2>"$T/cold.err"
find "$T/cache/maps" -name '*.truth' -type f | grep -q . || {
    echo "   no persisted truth sidecar was written"
    exit 1
}
grep -q "truth signed by the operator key" "$T/cold.err" || {
    cat "$T/cold.err"; exit 1;
}
echo "   ✓ signed truth persisted beside the map"

stop_swarm

echo "· intact signed warm cache remains usable offline"
TRACKER_ENV= run_shim >"$T/warm.out" 2>"$T/warm.err"
grep -q "offline: serving from the local mirror only" "$T/warm.err"
grep -q "using persisted signed integrity truth" "$T/warm.err" || {
    cat "$T/warm.err"; exit 1;
}
echo "   ✓ warm bytes were checked using persisted truth"

echo "· an altered warm byte is repaired offline from the local content store"
printf '\xff' | dd of="$T/cache/data/w.bin" bs=1 seek=17 conv=notrunc status=none
TRACKER_ENV= run_shim >"$T/casheal.out" 2>"$T/casheal.err"
grep -q "local integrity failure: block 0 of w.bin" "$T/casheal.err" || {
    cat "$T/casheal.err"; exit 1;
}
cmp "$T/src/w.bin" "$T/cache/data/w.bin"
echo "   ✓ persisted truth let the CAS restore signed bytes with no network"

echo "· altered warm byte fails with a named EIO before wrong output"
printf '\xfd' | dd of="$T/cache/data/w.bin" bs=1 seek=17 conv=notrunc status=none
set +e
CAS_DIR="$T/nocas" TRACKER_ENV= run_shim >"$T/corrupt.out" 2>"$T/corrupt.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   altered bytes were returned successfully"; exit 1; }
grep -q "pread virtual (w.bin): Input/output error" "$T/corrupt.err" || {
    echo "   corruption did not surface as intentional EIO"
    cat "$T/corrupt.err"
    exit 1
}
grep -q "local integrity failure: block 0 of w.bin" "$T/corrupt.err" || {
    echo "   integrity failure did not name file and block"
    cat "$T/corrupt.err"
    exit 1
}
if grep -q "bytes differ" "$T/corrupt.err"; then
    echo "   wrong bytes escaped before the failure"
    exit 1
fi
echo "   ✓ corrupted local bytes never reached the engine"

echo "· an online peer repairs the invalidated block"
start_swarm
TRACKER_ENV=LUMABRI_TRACKER=127.0.0.1:7590 CAS_DIR="$T/nocas" \
    run_shim >"$T/repair.out" 2>"$T/repair.err"
cmp "$T/src/w.bin" "$T/cache/data/w.bin"
echo "   ✓ bad presence bit was cleared and the peer restored correct bytes"

echo "· two processes can validate and repair one shared cache"
printf '\xfe' | dd of="$T/cache/data/w.bin" bs=1 seek=1048600 conv=notrunc status=none
TRACKER_ENV=LUMABRI_TRACKER=127.0.0.1:7590 run_shim >"$T/p1.out" 2>"$T/p1.err" & A=$!
TRACKER_ENV=LUMABRI_TRACKER=127.0.0.1:7590 run_shim >"$T/p2.out" 2>"$T/p2.err" & B=$!
wait "$A"
wait "$B"
cmp "$T/src/w.bin" "$T/cache/data/w.bin"
echo "   ✓ both processes returned correct bytes from the coordinated repair"

stop_swarm

echo "· a different public key rejects the persisted mirror outright"
set +e
PUBKEY=$(cat "$T/other.pub") TRACKER_ENV= run_shim >"$T/wrong.out" 2>"$T/wrong.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   wrong key accepted persisted state"; exit 1; }
grep -q "offline mirror has no verified model identity" "$T/wrong.err" || {
    cat "$T/wrong.err"; exit 1;
}
if grep -q "bytes differ" "$T/wrong.err"; then
    echo "   wrong bytes escaped under the wrong key"
    exit 1
fi
echo "   ✓ out-of-band authority remains decisive"

echo "· a malformed public key fails closed instead of disabling verification"
set +e
PUBKEY=not-a-public-key TRACKER_ENV= run_shim >"$T/malformed.out" 2>"$T/malformed.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   malformed key silently enabled unsigned mode"; exit 1; }
grep -q "not a valid key/keyring — refusing the model" "$T/malformed.err" || {
    cat "$T/malformed.err"; exit 1;
}
if grep -q "test_shim: PASS" "$T/malformed.out" ||
   grep -q "bytes differ" "$T/malformed.err"; then
    echo "   model bytes escaped after trust configuration failed"
    exit 1
fi
echo "   ✓ an explicit but invalid trust policy cannot fall back to unsigned mode"

echo "· an explicitly empty public key also fails closed"
set +e
PUBKEY= TRACKER_ENV= run_shim >"$T/empty-key.out" 2>"$T/empty-key.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   empty key silently enabled unsigned mode"; exit 1; }
grep -q "not a valid key/keyring — refusing the model" "$T/empty-key.err" || {
    cat "$T/empty-key.err"; exit 1;
}
echo "   ✓ an empty override cannot suppress trust verification"

echo "· a malformed keyring file fails closed"
printf 'not-a-public-key\n' > "$T/malformed.pub"
set +e
PUBKEY="$T/malformed.pub" TRACKER_ENV= \
    run_shim >"$T/malformed-file.out" 2>"$T/malformed-file.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   malformed keyring enabled unsigned mode"; exit 1; }
grep -q "not a valid key/keyring — refusing the model" "$T/malformed-file.err" || {
    cat "$T/malformed-file.err"; exit 1;
}
echo "   ✓ unreadable trust contents cannot weaken the configured policy"

echo "· one valid literal cannot hide a malformed key beside it"
set +e
PUBKEY="$PUB,not-a-public-key" TRACKER_ENV= \
    run_shim >"$T/partial-key.out" 2>"$T/partial-key.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   partially malformed trust set was accepted"; exit 1; }
grep -q "not a valid key/keyring — refusing the model" "$T/partial-key.err" || {
    cat "$T/partial-key.err"; exit 1;
}
echo "   ✓ trust parsing is atomic: any bad member rejects the whole set"

echo "· REQUIRE_HASH without an out-of-band key rejects cached sidecars"
set +e
env -u LUMABRI_PUBKEY LD_PRELOAD="$PWD/liblumabri.so" \
    LUMABRI_VROOT="$T/v" LUMABRI_CACHE="$T/cache" LUMABRI_MODEL=fx \
    LUMABRI_BLOCK_MIB=1 LUMABRI_REQUIRE_HASH=1 LUMABRI_CAS="$T/nocas" \
    ./test_shim "$T/v" "$T/src" >"$T/require.out" 2>"$T/require.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   REQUIRE_HASH trusted an unauthenticated sidecar"; exit 1; }
grep -q "persisted truth exists but is unusable" "$T/require.err" || {
    cat "$T/require.err"; exit 1;
}
grep -q "integrity required but unavailable" "$T/require.err" || {
    cat "$T/require.err"; exit 1;
}
echo "   ✓ strict policy cannot promote a locally rewriteable sidecar"

echo "· changing block size cannot reinterpret the old map geometry"
set +e
BLOCK_MIB=2 TRACKER_ENV= run_shim >"$T/geometry.out" 2>"$T/geometry.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   old map was reinterpreted at a new geometry"; exit 1; }
grep -q "offline mirror has no verified model identity" "$T/geometry.err" || {
    cat "$T/geometry.err"; exit 1;
}
echo "   ✓ geometry mismatch cannot promote partial old blocks"

echo "· a missing data file cannot turn presence bits into sparse zeros"
# Reseed an intact cache first, then take a legacy copy for the last scenario.
start_swarm
BLOCK_MIB=1 TRACKER_ENV=LUMABRI_TRACKER=127.0.0.1:7590 run_shim >/dev/null 2>"$T/reseed.err"
cp -a "$T/cache" "$T/legacy-cache"
stop_swarm
rm "$T/cache/data/w.bin"
set +e
CAS_DIR="$T/nocas" TRACKER_ENV= run_shim >"$T/missing.out" 2>"$T/missing.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   deleted mirror was served as sparse zeros"; exit 1; }
grep -q "no peer could serve it" "$T/missing.err" || {
    cat "$T/missing.err"; exit 1;
}
if grep -q "bytes differ" "$T/missing.err"; then
    echo "   sparse zeros escaped before EIO"
    exit 1
fi
echo "   ✓ absent mirror data was refetched or refused, never invented"

echo "· the same missing data heals offline when the content store survives"
TRACKER_ENV= run_shim >"$T/heal.out" 2>"$T/heal.err"
cmp "$T/src/w.bin" "$T/cache/data/w.bin"
echo "   ✓ persisted truth reassembled the mirror from local verified chunks"

echo "· legacy unsigned offline caches remain compatible with a loud warning"
find "$T/legacy-cache/maps" -name '*.truth' -delete
env -u LUMABRI_PUBKEY LD_PRELOAD="$PWD/liblumabri.so" \
    LUMABRI_VROOT="$T/v" LUMABRI_CACHE="$T/legacy-cache" LUMABRI_MODEL=fx \
    LUMABRI_BLOCK_MIB=1 LUMABRI_CAS="$T/nocas" ./test_shim "$T/v" "$T/src" \
    >"$T/legacy.out" 2>"$T/legacy.err"
grep -q "test_shim: PASS" "$T/legacy.out" || {
    cat "$T/legacy.out" "$T/legacy.err"; exit 1;
}
grep -q "WARNING: legacy warm cache has no authenticated truth" "$T/legacy.err" || {
    cat "$T/legacy.err"; exit 1;
}
echo "   ✓ compatibility preserved only where no strict policy was stated"

echo "LUMABRI HOT CACHE INTEGRITY TEST: PASS"
