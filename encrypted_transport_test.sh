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
#   3) the model bytes never appear on the wire in the clear;
#   4) a wrong endpoint pin is rejected before any application frame.
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
# A user-space tap sits in front of the maintainer: the maintainer listens on
# 7641 but advertises the tap's port, so every chatter reaches it through the
# tap, whose log is an exact copy of what crosses that link. This needs no
# tcpdump and no root, so the "never in the clear" check below runs everywhere,
# not only where a raw-capture tool happens to be installed.
python3 wire_tap.py --listen 7642 --to 127.0.0.1:7641 --log "$T/wire.log" \
    > "$T/tap.out" 2>&1 & PIDS+=($!)
for _ in $(seq 1 100); do grep -q ready "$T/tap.out" && break; sleep 0.1; done
env LUMABRI_ENCRYPT=1 ./maintainer --root "$T/src" --port 7641 \
    --tracker 127.0.0.1:7640 --name origin --model-name fx \
    --advertise 127.0.0.1:7642 > "$T/origin.log" 2>&1 & PIDS+=($!)
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
# A fresh encrypted fetch through the tap, then grep its log for the marker.
# The successful fetch in step 1 already carried block 0 (where the marker is)
# across the tap, but do it again from a cold cache so the check does not
# depend on step 1's ordering.
env LUMABRI_ENCRYPT=1 LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v3" \
    LUMABRI_CACHE="$T/c3" LUMABRI_TRACKER=127.0.0.1:7640 LUMABRI_MODEL=fx \
    LUMABRI_BLOCK_MIB=1 LUMABRI_HS_TIMEOUT_MS=2000 \
    timeout 30 ./test_shim "$T/v3" "$T/src" > "$T/shim3.log" 2>&1
grep -q "byte-identical" "$T/shim3.log" || { echo "   encrypted refetch failed"; cat "$T/shim3.log"; exit 1; }
if grep -a -q "$MARKER" "$T/wire.log"; then
    echo "   the model marker appeared on the wire in the clear"; exit 1; fi
# The marker did cross this link (step 1 served it byte-for-byte); it simply
# never appeared as plaintext. Confirm the tap actually observed traffic, so a
# silently empty log can never pass this check.
[ -s "$T/wire.log" ] || { echo "   the tap logged nothing — capture is not proving anything"; exit 1; }
echo "   ✓ the marker is not in the tapped ciphertext ($(wc -c < "$T/wire.log") bytes observed)"

# Where a raw-capture tool is available, confirm the same at the kernel wire as
# well — belt and braces, but never the only check.
if command -v tcpdump >/dev/null 2>&1 && tcpdump -D >/dev/null 2>&1; then
    tcpdump -i lo -w "$T/cap.pcap" "tcp port 7641" > /dev/null 2>&1 & TD=$!
    sleep 0.5
    if kill -0 "$TD" 2>/dev/null; then
        env LUMABRI_ENCRYPT=1 LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v4" \
            LUMABRI_CACHE="$T/c4" LUMABRI_TRACKER=127.0.0.1:7640 LUMABRI_MODEL=fx \
            LUMABRI_BLOCK_MIB=1 LUMABRI_HS_TIMEOUT_MS=2000 timeout 30 ./test_shim "$T/v4" "$T/src" > /dev/null 2>&1
        sleep 0.5; kill "$TD" 2>/dev/null || true; wait "$TD" 2>/dev/null || true
        if [ ! -s "$T/cap.pcap" ] || ! tcpdump -r "$T/cap.pcap" -c 1 >/dev/null 2>&1; then
            echo "   · kernel capture skipped: tcpdump produced no readable packets"
        elif grep -a -q "$MARKER" "$T/cap.pcap"; then
            echo "   the model marker appeared on the raw wire in the clear"; exit 1
        else
            echo "   ✓ confirmed at the kernel wire too (tcpdump)"
        fi
    else
        wait "$TD" 2>/dev/null || true
        echo "   · kernel capture skipped: tcpdump could not start"
    fi
else
    echo "   · kernel capture skipped: tcpdump unavailable (user-space tap passed)"
fi

echo "· 4) a wrong endpoint pin is rejected"
printf '127.0.0.1:7640 %064d\n' 0 > "$T/wrong.pins"
set +e
env LUMABRI_ENCRYPT=1 LUMABRI_PEER_PINS="$T/wrong.pins" \
    LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v5" LUMABRI_CACHE="$T/c5" \
    LUMABRI_TRACKER=127.0.0.1:7640 LUMABRI_MODEL=fx LUMABRI_BLOCK_MIB=1 \
    LUMABRI_HS_TIMEOUT_MS=2000 timeout 20 ./test_shim "$T/v5" "$T/src" \
    > "$T/wrong-pin.log" 2>&1
PIN_RC=$?
set -e
if [ "$PIN_RC" -eq 0 ] && grep -q "byte-identical" "$T/wrong-pin.log"; then
    echo "   client accepted the wrong tracker identity pin"; exit 1
fi
grep -q '^127\.0\.0\.1:7640 ' "$T/.lumabri/known_hosts" || {
    echo "   tracker identity was not persisted in known_hosts"; exit 1; }
echo "   ✓ wrong pin refused; successful TOFU identity persisted"

echo "· 5) encryption cannot downgrade when its identity key is unavailable"
set +e
env HOME="$T" LUMABRI_ENCRYPT=1 \
    LUMABRI_PEER_KEY="$T/missing-parent/peer.key" \
    ./tracker --port 7649 > "$T/fail-closed.log" 2>&1
KEY_RC=$?
set -e
if [ "$KEY_RC" -eq 0 ]; then
    echo "   tracker continued after encrypted identity setup failed"; exit 1
fi
grep -q "refusing all network traffic" "$T/fail-closed.log" || {
    echo "   encrypted identity failure was not explicit"; cat "$T/fail-closed.log"; exit 1; }
echo "   ✓ startup refused; no plaintext listener was opened"

echo "LUMABRI ENCRYPTED TRANSPORT TEST: PASS"
