#!/usr/bin/env bash
# lumabri — the transport carries model bytes, encrypted, end to end.
#
# With LUMABRI_ENCRYPT set on every process, a tracker, a maintainer and a
# chatter handshake (X25519 + Ed25519 identity) and every frame after that is
# ChaCha20-Poly1305. This proves:
#
#   1) an encrypted swarm serves a model byte-for-byte over the AEAD channel;
#   2) a plaintext chatter against an encrypted maintainer gets nothing, not
#      the bytes in the clear — encryption is not silently skippable;
#   3) the model bytes never appear on the wire in the clear.
set -euo pipefail
cd "$(dirname "$0")"

make -s tracker maintainer liblumabri.so test_shim

T=$(mktemp -d /tmp/lumabri-enc.XXXXXX)
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

mkdir -p "$T/src"
# a recognisable marker in the file, to grep for on the wire
MARKER="PLAINTEXT_MODEL_SECRET_$RANDOM$RANDOM"
printf '%s' "$MARKER" > "$T/src/w.bin"
head -c $((256 * 1024)) /dev/urandom >> "$T/src/w.bin"
printf '{"model_type":"olmoe"}\n' > "$T/src/config.json"
export HOME="$T"          # peer keys land under $T/.lumabri, isolated

echo "· 1) an encrypted swarm serves the model byte-for-byte"
env LUMABRI_ENCRYPT=1 ./tracker --port 7640 > "$T/tracker.log" 2>&1 & PIDS+=($!)
wait_port 7640
env LUMABRI_ENCRYPT=1 ./maintainer --root "$T/src" --port 7641 \
    --tracker 127.0.0.1:7640 --name origin --model-name fx > "$T/origin.log" 2>&1 & PIDS+=($!)
wait_port 7641
sleep 1
env LUMABRI_ENCRYPT=1 LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v" \
    LUMABRI_CACHE="$T/c" LUMABRI_TRACKER=127.0.0.1:7640 LUMABRI_MODEL=fx \
    LUMABRI_BLOCK_MIB=1 LUMABRI_HS_TIMEOUT_MS=2000 \
    timeout 30 ./test_shim "$T/v" "$T/src" > "$T/shim.log" 2>&1
grep -q "byte-identical" "$T/shim.log" || { echo "   encrypted fetch failed"; cat "$T/shim.log"; exit 1; }
grep -q "encryption on" "$T/origin.log" || { echo "   maintainer did not enable encryption"; exit 1; }
echo "   ✓ served byte-for-byte over X25519 + ChaCha20-Poly1305"

echo "· 2) a plaintext chatter against the encrypted swarm gets nothing"
set +e
env LMB_TIMEOUT= LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v2" \
    LUMABRI_CACHE="$T/c2" LUMABRI_TRACKER=127.0.0.1:7640 LUMABRI_MODEL=fx \
    LUMABRI_BLOCK_MIB=1 LUMABRI_IO_TIMEOUT_MS=2000 \
    timeout 20 ./test_shim "$T/v2" "$T/src" > "$T/plain.log" 2>&1
RC=$?
set -e
if [ "$RC" -eq 0 ] && grep -q "byte-identical" "$T/plain.log"; then
    echo "   a plaintext chatter was served by an encrypted maintainer"; cat "$T/plain.log"; exit 1
fi
echo "   ✓ refused: encryption cannot be silently skipped"

echo "· 3) the model bytes never crossed the wire in the clear"
# capture the maintainer's traffic on a fresh encrypted fetch and grep the marker
if command -v tcpdump >/dev/null 2>&1 && tcpdump -D >/dev/null 2>&1; then
    tcpdump -i lo -w "$T/cap.pcap" "tcp port 7641" > /dev/null 2>&1 & TD=$!
    sleep 0.5
    env LUMABRI_ENCRYPT=1 LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v3" \
        LUMABRI_CACHE="$T/c3" LUMABRI_TRACKER=127.0.0.1:7640 LUMABRI_MODEL=fx \
        LUMABRI_BLOCK_MIB=1 LUMABRI_HS_TIMEOUT_MS=2000 timeout 30 ./test_shim "$T/v3" "$T/src" > /dev/null 2>&1
    sleep 0.5; kill "$TD" 2>/dev/null || true; wait "$TD" 2>/dev/null || true
    if grep -a -q "$MARKER" "$T/cap.pcap"; then
        echo "   the model marker appeared on the wire in the clear"; exit 1; fi
    echo "   ✓ the marker is not in the captured ciphertext"
else
    echo "   (skipped wire capture: tcpdump unavailable in this environment)"
fi

echo "LUMABRI ENCRYPTED TRANSPORT TEST: PASS"
