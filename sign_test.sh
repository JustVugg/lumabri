#!/usr/bin/env bash
# lumabri — the crypto, checked against the world and against an attacker.
#
#   1) SHA-512   matches the system sha512sum.
#   2) Ed25519   RFC 8032 vector, and OpenSSL agrees in BOTH directions:
#                what we sign verifies there, what it signs verifies here.
#                (skipped if this box has no openssl with ed25519)
#   3) SWARM     a signed swarm end to end: the origin signs, the tracker
#                carries, the chatter verifies with the public key alone —
#                and a tracker that rewrites the truth is caught, which is
#                the whole point of signing it.
set -euo pipefail
cd "$(dirname "$0")"

make -s all
T=$(mktemp -d /tmp/lumabri-sign.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

cat > "$T/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "lumabri_sign.h"
int main(int argc, char **argv) {
    char hx[200];
    uint8_t h[64], seed[32], pk[32], sk[64], sig[64];
    if (!strcmp(argv[1], "sha512")) {
        lmb_sha512(argv[2], strlen(argv[2]), h);
        lmb_hex(hx, h, 64); puts(hx); return 0;
    }
    if (!strcmp(argv[1], "vector")) {          /* RFC 8032 test 2 */
        lmb_unhex(seed, "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", 32);
        lmb_sign_keypair(pk, sk, seed);
        uint8_t m[1] = {0x72};
        lmb_sign(sig, m, 1, sk);
        lmb_hex(hx, pk, 32); printf("%s\n", hx);
        lmb_hex(hx, sig, 64); printf("%s\n", hx);
        printf("%d\n", lmb_sign_verify(sig, m, 1, pk));
        sig[10] ^= 0x40;
        printf("%d\n", lmb_sign_verify(sig, m, 1, pk));
        return 0;
    }
    if (!strcmp(argv[1], "sign")) {            /* sign argv[3] with key file */
        char kh[200]; FILE *f = fopen(argv[2], "r");
        if (!f || fscanf(f, "%198s", kh) != 1) return 1;
        fclose(f); lmb_unhex(sk, kh, 64);
        lmb_sign(sig, argv[3], strlen(argv[3]), sk);
        lmb_hex(hx, sig, 64); puts(hx); return 0;
    }
    if (!strcmp(argv[1], "verify")) {          /* pubhex sighex msg */
        lmb_unhex(pk, argv[2], 32); lmb_unhex(sig, argv[3], 64);
        return lmb_sign_verify(sig, argv[4], strlen(argv[4]), pk) == 0 ? 0 : 1;
    }
    return 2;
}
EOF
cc -O2 -I. "$T/t.c" -o "$T/t"

echo "· 1) SHA-512 against the system implementation"
MINE=$("$T/t" sha512 "lumabri")
THEIRS=$(printf 'lumabri' | sha512sum | cut -d' ' -f1)
[ "$MINE" = "$THEIRS" ] || { echo "   MISMATCH"; exit 1; }
echo "   ✓ identical"

echo "· 2) Ed25519 against RFC 8032 and OpenSSL"
readarray -t V < <("$T/t" vector)
[ "${V[0]}" = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c" ] \
    || { echo "   public key does not match RFC 8032"; exit 1; }
[ "${V[1]}" = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" ] \
    || { echo "   signature does not match RFC 8032"; exit 1; }
[ "${V[2]}" = "0" ] || { echo "   own signature did not verify"; exit 1; }
[ "${V[3]}" != "0" ] || { echo "   a TAMPERED signature verified — fatal"; exit 1; }
echo "   ✓ RFC 8032 vector reproduced, tampering rejected"

if openssl genpkey -algorithm ed25519 -out "$T/k.pem" 2>/dev/null; then
    MSG="lumabri swarm ground truth"
    openssl pkey -in "$T/k.pem" -pubout -outform DER -out "$T/pub.der" 2>/dev/null
    OPUB=$(tail -c 32 "$T/pub.der" | xxd -p -c 64)
    printf '%s' "$MSG" > "$T/m.bin"
    openssl pkeyutl -sign -inkey "$T/k.pem" -rawin -in "$T/m.bin" -out "$T/s.bin" 2>/dev/null
    OSIG=$(xxd -p -c 128 "$T/s.bin")
    "$T/t" verify "$OPUB" "$OSIG" "$MSG" || { echo "   openssl→us FAILED"; exit 1; }
    echo "   ✓ openssl's signature verifies here"

    ./lumabri key --out "$T/op" > /dev/null
    OURSIG=$("$T/t" sign "$T/op.key" "$MSG")
    OURPUB=$(cat "$T/op.pub")
    printf '%s' "$OURSIG" | xxd -r -p > "$T/ours.bin"
    { printf '\x30\x2a\x30\x05\x06\x03\x2b\x65\x70\x03\x21\x00';
      printf '%s' "$OURPUB" | xxd -r -p; } > "$T/ourpub.der"
    openssl pkey -pubin -inform DER -in "$T/ourpub.der" -out "$T/ourpub.pem" 2>/dev/null
    if openssl pkeyutl -verify -pubin -inkey "$T/ourpub.pem" -rawin -in "$T/m.bin" \
            -sigfile "$T/ours.bin" >/dev/null 2>&1; then
        echo "   ✓ our signature verifies in openssl"
    else
        echo "   us→openssl FAILED"; exit 1
    fi
else
    echo "   (openssl ed25519 unavailable — cross-check skipped)"
fi

echo "· 3) signed swarm: origin signs, tracker carries, chatter verifies"
mkdir -p "$T/src"
head -c $((3 * 1024 * 1024)) /dev/urandom > "$T/src/w.bin"
head -c 512 /dev/urandom > "$T/src/config.json"
./lumabri key --out "$T/swarm" > /dev/null
PUB=$(cat "$T/swarm.pub")

./tracker --port 7460 --pubkey "$T/swarm.pub" > "$T/tracker.log" 2>&1 & PIDS+=($!)
sleep 0.3
./maintainer --root "$T/src" --port 7461 --tracker 127.0.0.1:7460 --name origin \
             --key "$T/swarm.key" > "$T/origin.log" 2>&1 & PIDS+=($!)
sleep 1.2
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v" LUMABRI_CACHE="$T/c" \
    LUMABRI_TRACKER=127.0.0.1:7460 LUMABRI_BLOCK_MIB=1 LUMABRI_PUBKEY="$PUB" \
    ./test_shim "$T/v" "$T/src" 2> "$T/shim.err"
grep -q "signed by the operator key" "$T/shim.err" || {
    echo "   SIGNED SWARM FAILED: the chatter never saw a valid signature"
    cat "$T/shim.err"; exit 1; }
echo "   ✓ bytes accepted only because the operator signed them"

echo "· 3b) an unsigned peer cannot get its files into a signed swarm"
mkdir -p "$T/rogue"
head -c $((3 * 1024 * 1024)) /dev/urandom > "$T/rogue/w.bin"
./maintainer --root "$T/rogue" --port 7462 --tracker 127.0.0.1:7460 \
             --name rogue --model-name src > "$T/rogue.log" 2>&1 & PIDS+=($!)
sleep 1.5
grep -q "REJECTED: rogue" "$T/tracker.log" || {
    echo "   REJECTION FAILED: the tracker accepted unsigned truth"
    cat "$T/tracker.log"; exit 1; }
echo "   ✓ tracker refused the unsigned claim"

echo "· 3c) a LYING tracker is caught by the chatter's own key"
# the rogue tracker has no operator key and invents its own truth: it takes
# the rogue's (different) bytes as gospel and serves them unsigned
./tracker --port 7463 > "$T/evil.log" 2>&1 & PIDS+=($!)
sleep 0.3
./maintainer --root "$T/rogue" --port 7464 --tracker 127.0.0.1:7463 \
             --name rogue2 --model-name src > "$T/rogue2.log" 2>&1 & PIDS+=($!)
sleep 1.2
set +e
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v2" LUMABRI_CACHE="$T/c2" \
    LUMABRI_TRACKER=127.0.0.1:7463 LUMABRI_BLOCK_MIB=1 LUMABRI_PUBKEY="$PUB" \
    ./test_shim "$T/v2" "$T/rogue" > /dev/null 2> "$T/evil_shim.err"
RC=$?
set -e
grep -q "not signed by the operator key" "$T/evil_shim.err" || {
    echo "   LYING TRACKER UNPROVEN: no signature complaint"
    cat "$T/evil_shim.err"; exit 1; }
[ "$RC" -ne 0 ] || { echo "   LYING TRACKER FAILED: the chatter used its bytes"; exit 1; }
echo "   ✓ unsigned truth refused: the tracker is a courier, not an authority"

echo "LUMABRI SIGN TEST: PASS"
