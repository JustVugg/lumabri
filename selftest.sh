#!/usr/bin/env bash
# lumibri end-to-end selftest — no model needed, runs in seconds.
#
# Builds a small random tree, serves it from TWO maintainers holding disjoint
# halves, and verifies through the LD_PRELOAD shim that every byte read via
# the virtual root is identical to the source:
#   pass 1  cold   — every block crosses the network
#   pass 2  warm   — served from the local mirror
#   pass 3  offline — tracker and maintainers are DEAD; the warm mirror must
#                     still serve everything (the whole point of the design)
set -euo pipefail
cd "$(dirname "$0")"

make -s all

T=$(mktemp -d /tmp/lumibri-selftest.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

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
sleep 0.5

ENV=(LD_PRELOAD="$PWD/liblumibri.so"
     LUMIBRI_VROOT="$T/vroot"
     LUMIBRI_CACHE="$T/cache"
     LUMIBRI_TRACKER=127.0.0.1:7390
     LUMIBRI_BLOCK_MIB=1)

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
sleep 0.7
NATENV=(LD_PRELOAD="$PWD/liblumibri.so"
        LUMIBRI_VROOT="$T/vroot2"
        LUMIBRI_CACHE="$T/cache2"
        LUMIBRI_TRACKER=127.0.0.1:7390
        LUMIBRI_BLOCK_MIB=1)
env "${NATENV[@]}" ./test_shim "$T/vroot2" "$T/src"

echo "LUMIBRI SELFTEST: PASS"
