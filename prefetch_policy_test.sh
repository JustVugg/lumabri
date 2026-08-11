#!/usr/bin/env bash
# lumabri — readahead must stop at the dense/expert line.
#
# The byte-mirror reads ahead to hide latency: touch a block and the next few
# arrive before they are asked for. That is right while a chatter is pulling a
# whole model. It is wrong the moment the experts run on peers, because at
# 1 MiB granularity a readahead past a dense block walks into the adjacent
# expert region and pulls weights the chatter will never execute — a little
# more on every run.
#
# This reads exactly ONE block of an eight-block file and counts how many the
# mirror actually fetched, in three worlds:
#
#   1) default            — readahead on, more than one block arrives;
#   2) remote experts      — the engine signalled delegation, only the one
#                            requested block arrives;
#   3) remote experts + an explicit LUMABRI_PREFETCH — the operator's choice
#                            wins over the automatic suppression.
set -euo pipefail
cd "$(dirname "$0")"

make -s liblumabri.so tracker maintainer

T=$(mktemp -d /tmp/lumabri-prefetch.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

wait_port() {
    for _ in $(seq 1 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    return 1
}

# eight distinct 1 MiB blocks so a readahead is unambiguous
mkdir -p "$T/src"
head -c $((8 * 1024 * 1024)) /dev/urandom > "$T/src/w.bin"
printf '{"model_type":"olmoe"}\n' > "$T/src/config.json"

# a reader that touches only the first block, waits for any readahead to run,
# then leaves — so the mirror map reflects exactly what the policy fetched
cat > "$T/touch1.c" <<'EOF'
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) return 1;
    char buf[4096];
    if (pread(fd, buf, sizeof buf, 0) != sizeof buf) return 1;
    usleep(700000);            /* let prefetch workers drain, if any */
    close(fd);
    return 0;
}
EOF
cc -O2 -w "$T/touch1.c" -o "$T/touch1"

./tracker --port 7620 > "$T/tracker.log" 2>&1 & PIDS+=($!)
wait_port 7620
./maintainer --root "$T/src" --port 7621 --tracker 127.0.0.1:7620 \
             --name origin --model-name fx > "$T/origin.log" 2>&1 & PIDS+=($!)
wait_port 7621
sleep 1

# count present blocks in the mirror map for w.bin (one byte per block, 1=present)
present() {
    python3 - "$1/maps/w.bin.lmap" <<'PY'
import sys
try:
    d = open(sys.argv[1], "rb").read()
except FileNotFoundError:
    print(0); raise SystemExit
print(sum(1 for b in d if b))
PY
}

run() {   # run VROOT CACHE  [extra env...]
    local v=$1 c=$2; shift 2
    env "$@" LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$v" \
        LUMABRI_CACHE="$c" LUMABRI_TRACKER=127.0.0.1:7620 \
        LUMABRI_MODEL=fx LUMABRI_BLOCK_MIB=1 \
        "$T/touch1" "$v/w.bin" > /dev/null 2>&1
}

echo "· 1) default: readahead pulls more than the one block touched"
run "$T/v1" "$T/c1"
N1=$(present "$T/c1")
echo "   fetched $N1 block(s)"
[ "$N1" -gt 1 ] || { echo "   readahead did not run — test cannot distinguish the cases"; exit 1; }

echo "· 2) remote experts: only the requested block is fetched"
run "$T/v2" "$T/c2" LUMABRI_REMOTE_EXPERTS=1
N2=$(present "$T/c2")
echo "   fetched $N2 block(s)"
[ "$N2" -eq 1 ] || { echo "   readahead still crossed into unrequested blocks"; exit 1; }

echo "· 3) an explicit prefetch depth wins over the automatic suppression"
run "$T/v3" "$T/c3" LUMABRI_REMOTE_EXPERTS=1 LUMABRI_PREFETCH=3
N3=$(present "$T/c3")
echo "   fetched $N3 block(s)"
[ "$N3" -gt 1 ] || { echo "   an explicit override was ignored"; exit 1; }

echo "LUMABRI PREFETCH POLICY TEST: PASS"
