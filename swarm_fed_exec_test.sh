#!/usr/bin/env bash
# A compute donor with no model on disk. A tracker+maintainer hold tiny_olmoe;
# one expert_node reads the container from disk (the truth), a second runs
# behind the shim with --model pointing into the model's vroot — every loader
# read becomes a verified block fetch from the swarm, so it pulls exactly the
# experts it holds. The same EXEC against both must be byte-identical.
set -euo pipefail
cd "$(dirname "$0")"

make -s tracker maintainer liblumabri.so expert_node test_swarm_fed fixture

T=$(mktemp -d /tmp/lumabri-swarmfed.XXXXXX)
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
    for _ in $(seq 1 300); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    echo "port $1 never opened"
    return 1
}

./tracker --port 7591 >"$T/tracker.log" 2>&1 & PIDS+=($!)
./maintainer --root tiny_olmoe --port 7592 --tracker 127.0.0.1:7591 \
    --name sf-origin --model-name tiny-olmoe >"$T/maint.log" 2>&1 & PIDS+=($!)
wait_port 7591
wait_port 7592
sleep .3

echo "── A) the truth: an expert node reading the container from disk"
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
./expert_node --model tiny_olmoe --port 7593 --name sf-local \
    >"$T/local.log" 2>&1 & PIDS+=($!)
wait_port 7593 || { tail -20 "$T/local.log"; exit 1; }

echo "── B) the same node with NO model on disk: fed through the swarm mirror"
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_TRACKER=127.0.0.1:7591 \
    LUMABRI_MODEL=tiny-olmoe LUMABRI_VROOT="$T/vroot" \
    LUMABRI_CACHE="$T/cache" LUMABRI_CAS="$T/cas" PIN=0 \
./expert_node --model "$T/vroot" --port 7594 --name sf-fed \
    >"$T/fed.log" 2>&1 & PIDS+=($!)
wait_port 7594 || { tail -20 "$T/fed.log"; exit 1; }

# the vroot must have stayed virtual: the fed node never had a container
[ ! -e "$T/vroot" ] || { echo "✗ the vroot materialised on disk"; exit 1; }

DIM=$(sed -n 's/.*"hidden_size"[: ]*\([0-9]*\).*/\1/p' tiny_olmoe/config.json)
./test_swarm_fed 127.0.0.1:7593 127.0.0.1:7594 "$DIM" 0 1
./test_swarm_fed 127.0.0.1:7593 127.0.0.1:7594 "$DIM" 15 7
echo "✓ SWARM-FED DONOR: identical experts with no model on the donor's disk"
