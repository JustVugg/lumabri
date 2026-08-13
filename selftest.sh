#!/usr/bin/env bash
# lumabri end-to-end selftest — no model needed, runs in seconds.
#
# Builds a small random tree, serves it from TWO maintainers holding disjoint
# halves, and verifies through the LD_PRELOAD shim that every byte read via
# the virtual root is identical to the source:
#   pass 1  cold   — every block crosses the network
#   pass 2  warm   — served from the local mirror
#   pass 3  offline — tracker and maintainers are DEAD; the warm mirror must
#                     still serve everything (the whole point of the design)
#   pass 5  replaced — same path and size, different bytes; the complete-model
#                     identity must invalidate every old block map
set -euo pipefail
cd "$(dirname "$0")"

make -s all

T=$(mktemp -d /tmp/lumabri-selftest.XXXXXX)
export LUMABRI_PEER_BINDINGS="$T/peer-bindings"
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

wait_port() {
    for _ in $(seq 1 200); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    return 1
}

mkdir -p "$T/src/sub" "$T/cache"
head -c $((3 * 1024 * 1024 + 12345)) /dev/urandom > "$T/src/a.bin"
head -c $((2 * 1024 * 1024))        /dev/urandom > "$T/src/sub/b.bin"
head -c 777                          /dev/urandom > "$T/src/config.json"
: > "$T/src/empty.bin"

./tracker --port 7390 & PIDS+=($!)
sleep 0.3
./maintainer --root "$T/src" --port 7391 --tracker 127.0.0.1:7390 \
             --name half-a --include 'a.bin' --include '*.json' & PIDS+=($!)
./maintainer --root "$T/src" --port 7392 --tracker 127.0.0.1:7390 \
             --name half-b --include 'sub/*' --include 'empty.bin' & PIDS+=($!)
wait_port 7391; wait_port 7392
sleep 0.5

ENV=(LD_PRELOAD="$PWD/liblumabri.so"
     LUMABRI_VROOT="$T/vroot"
     LUMABRI_CACHE="$T/cache"
     LUMABRI_TRACKER=127.0.0.1:7390
     LUMABRI_BLOCK_MIB=1)

echo "· pass 1: cold (all blocks over the network)"
env "${ENV[@]}" ./test_shim "$T/vroot" "$T/src"

echo "· pass 2: warm (local mirror)"
env "${ENV[@]}" ./test_shim "$T/vroot" "$T/src"

echo "· pass 3: offline (peers and tracker dead, warm mirror only)"
kill "${PIDS[@]}" 2>/dev/null || true
wait 2>/dev/null || true
PIDS=()
sleep 0.2
env "${ENV[@]}" ./test_shim "$T/vroot" "$T/src"

echo "· pass 4: NAT (peers advertise a dead address; bytes must flow via relay)"
./tracker --port 7390 & PIDS+=($!)
sleep 0.3
./maintainer --root "$T/src" --port 7391 --tracker 127.0.0.1:7390 \
             --name nat-a --advertise 127.0.0.1:1 \
             --include 'a.bin' --include '*.json' & PIDS+=($!)
./maintainer --root "$T/src" --port 7392 --tracker 127.0.0.1:7390 \
             --name nat-b --advertise 127.0.0.1:1 \
             --include 'sub/*' --include 'empty.bin' & PIDS+=($!)
wait_port 7391; wait_port 7392
sleep 0.5
NATENV=(LD_PRELOAD="$PWD/liblumabri.so"
        LUMABRI_VROOT="$T/vroot2"
        LUMABRI_CACHE="$T/cache2"
        LUMABRI_TRACKER=127.0.0.1:7390
        LUMABRI_BLOCK_MIB=1)
env "${NATENV[@]}" ./test_shim "$T/vroot2" "$T/src"

echo "· pass 5: same-size checkpoint replacement invalidates the warm mirror"
kill "${PIDS[@]}" 2>/dev/null || true
wait 2>/dev/null || true
PIDS=()

# First bind a fully warm mirror to the identity of the complete model.  The
# earlier split maintainers intentionally hold only slices and therefore
# cannot originate a complete-model root.
./tracker --port 7390 & PIDS+=($!)
sleep 0.3
./maintainer --root "$T/src" --port 7391 --tracker 127.0.0.1:7390 \
             --name complete --model-name src & PIDS+=($!)
wait_port 7391
sleep 0.5
IDENV=(LD_PRELOAD="$PWD/liblumabri.so"
       LUMABRI_VROOT="$T/vroot3"
       LUMABRI_CACHE="$T/cache3"
       LUMABRI_TRACKER=127.0.0.1:7390
       LUMABRI_MODEL=src
       LUMABRI_BLOCK_MIB=1)
env "${IDENV[@]}" ./test_shim "$T/vroot3" "$T/src" >/dev/null

kill "${PIDS[@]}" 2>/dev/null || true
wait 2>/dev/null || true
PIDS=()
ASZ=$(stat -c %s "$T/src/a.bin")
head -c "$ASZ" /dev/urandom > "$T/a.new"
mv "$T/a.new" "$T/src/a.bin"             # new bytes, exactly the same size

./tracker --port 7390 & PIDS+=($!)
sleep 0.3
./maintainer --root "$T/src" --port 7391 --tracker 127.0.0.1:7390 \
             --name complete --model-name src & PIDS+=($!)
wait_port 7391
sleep 0.5
env "${IDENV[@]}" ./test_shim "$T/vroot3" "$T/src" \
    > "$T/replaced.out" 2> "$T/replaced.err"
grep -q "model identity changed" "$T/replaced.err" || {
    echo "   REPLACEMENT FAILED: old cache identity was not invalidated"
    cat "$T/replaced.err"
    exit 1
}
echo "   ✓ new model root detected; stale maps cleared and bytes refetched"

echo "LUMABRI SELFTEST: PASS"
