#!/usr/bin/env bash
# A persistent disk donor starts with no model at all. The tracker assigns its
# slice, the donor pulls it, and that copy must keep serving after the origin
# disappears. This exercises the documented `lumabri serve --join --donate`
# command rather than starting the maintainer directly.
set -euo pipefail
cd "$(dirname "$0")"

make -s all

T=$(mktemp -d /tmp/lumabri-donate.XXXXXX)
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
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    return 1
}

wait_placement() {
    local addr=$1 model=$2 want_peer=$3
    for _ in $(seq 1 100); do
        if "$T/placement" "$addr" "$model" "$want_peer"; then return 0; fi
        sleep 0.1
    done
    return 1
}

cat > "$T/placement.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    LmbBuf b = {0}; LmbMsg m = {0};
    lmb_buf_str(&b, argv[2]);
    if (lmb_request(argv[1], LMB_PLACEMENT, b.p, (uint32_t)b.len, &m) ||
        m.op != LMB_PLACEMENT_R) return 1;
    LmbCur c = { m.body, m.body_len, 0 }; uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) return 1;
    for (uint32_t i = 0; i < n; i++) {
        char path[LMB_PATH_MAX], peer[64]; uint64_t size; uint16_t np;
        if (lmb_cur_str(&c, path, sizeof path) || lmb_cur_u64(&c, &size) ||
            lmb_cur_u16(&c, &np)) return 1;
        for (uint16_t p = 0; p < np; p++) {
            if (lmb_cur_str(&c, peer, sizeof peer)) return 1;
            if (!strcmp(peer, argv[3])) return 0;
        }
    }
    return 1;
}
EOF
cc -O2 -w -I. "$T/placement.c" -o "$T/placement" -lpthread

mkdir -p "$T/src/sub"
head -c $((2 * 1024 * 1024 + 123)) /dev/urandom > "$T/src/weights.bin"
head -c 4096 /dev/urandom > "$T/src/sub/tokenizer.bin"
printf '{"model_type":"olmoe"}\n' > "$T/src/config.json"

echo "· 1) an ordinary server still requires a model directory"
mkdir "$T/not-a-model"
set +e
./lumabri serve --model "$T/not-a-model" --no-exec > "$T/invalid.out" 2>&1
RC=$?
set -e
[ "$RC" -eq 2 ] && grep -q "config.json" "$T/invalid.out" || {
    echo "   serve accepted a directory that is not a model"; exit 1; }
echo "   ✓ rejected before starting any child"

for bad in 0 -1 nope nan inf 1e300; do
    set +e
    ./lumabri serve --model "$T/not-a-model" --join 127.0.0.1:1 \
        --model-name fixture --donate "$bad" > "$T/bad-donate.out" 2>&1
    RC=$?
    set -e
    [ "$RC" -eq 2 ] && grep -q "positive number" "$T/bad-donate.out" || {
        echo "   serve accepted invalid donation: $bad"; exit 1; }
done
echo "   ✓ invalid donation sizes cannot bypass model validation"
set +e
./lumabri serve --model "$T/not-a-model" --join "" --model-name fixture \
    --donate 1 > "$T/bad-join.out" 2>&1
JOIN_RC=$?
./lumabri serve --model "$T/not-a-model" --join 127.0.0.1:1 --model-name "" \
    --donate 1 > "$T/bad-name.out" 2>&1
NAME_RC=$?
set -e
[ "$JOIN_RC" -eq 2 ] && grep -q -- "--join TRACKER" "$T/bad-join.out" && \
    [ "$NAME_RC" -eq 2 ] && grep -q -- "--join TRACKER" "$T/bad-name.out" || {
    echo "   serve accepted an empty tracker or model name"; exit 1; }
echo "   ✓ an empty tracker or model name is not a donor identity"

echo "· 2) a disk donor may start from a directory that does not exist"
./tracker --port 7570 > "$T/tracker.log" 2>&1 & PIDS+=($!)
wait_port 7570
./maintainer --root "$T/src" --port 7571 --tracker 127.0.0.1:7570 \
    --name origin --model-name fixture > "$T/origin.log" 2>&1 & ORIGIN=$!; PIDS+=($!)
wait_port 7571
wait_placement 127.0.0.1:7570 fixture 127.0.0.1:7571
./lumabri serve --model "$T/donor" --join 127.0.0.1:7570 \
    --model-name fixture --donate 1 --port 7572 > "$T/donor.log" 2>&1 & DONOR=$!; PIDS+=($!)

READY=0
for _ in $(seq 1 200); do
    [ -f "$T/donor/config.json" ] && [ -f "$T/donor/weights.bin" ] && \
        [ -f "$T/donor/sub/tokenizer.bin" ] && { READY=1; break; }
    kill -0 "$DONOR" 2>/dev/null || { cat "$T/donor.log"; exit 1; }
    sleep 0.1
done
[ "$READY" -eq 1 ] || { echo "   donor did not pull its slice"; cat "$T/donor.log"; exit 1; }
wait_port 7573
wait_placement 127.0.0.1:7570 fixture 127.0.0.1:7573
cmp "$T/src/weights.bin" "$T/donor/weights.bin"
cmp "$T/src/sub/tokenizer.bin" "$T/donor/sub/tokenizer.bin"
cmp "$T/src/config.json" "$T/donor/config.json"
echo "   ✓ the tracker filled the empty slice byte-for-byte"

echo "· 3) the donated copy serves after the origin disappears"
kill "$ORIGIN" 2>/dev/null || true
wait "$ORIGIN" 2>/dev/null || true
PIDS=("${PIDS[0]}" "$DONOR")
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/vroot" \
    LUMABRI_CACHE="$T/cache" LUMABRI_TRACKER=127.0.0.1:7570 \
    LUMABRI_MODEL=fixture \
    LUMABRI_BLOCK_MIB=1 ./test_shim "$T/vroot" "$T/src"
echo "   ✓ the donor alone serves an identical model"

echo "LUMABRI DONATE TEST: PASS"
