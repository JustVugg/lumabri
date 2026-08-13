#!/usr/bin/env bash
# lumabri — donating disk to a swarm that is signed.
#
# This is the case that quietly stopped working. HASHES_R grew a model name
# and a signature when the tracker became a courier instead of an authority;
# the donor's parser did not, so it read the first bytes of the model string
# as a chunk size, decided the reply was malformed, and reported "no truth".
# And "no truth" meant "nothing to check against, so everything is fine".
#
# Two things followed, both silent. The donor pulled gigabytes without
# verifying a single byte. And, having no signature to republish, it
# announced unsigned bytes that the tracker refused — so it held nothing and
# donated nothing, while printing that it had pulled its slice.
#
# What this proves:
#
#   1) the donor verifies what it pulls, and says so;
#   2) the tracker accepts its announcement, because the origin's signature
#      travelled with the bytes;
#   3) the donated copy alone serves a chatter that checks the signature;
#   4) the signature survives a restart — it is on disk, not in RAM;
#   5) a donor holding the WRONG operator key refuses to hold anything,
#      which is the only evidence that any of the above is a check rather
#      than a claim.
set -euo pipefail
cd "$(dirname "$0")"

make -s all

T=$(mktemp -d /tmp/lumabri-sdonor.XXXXXX)
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
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    return 1
}

wait_for() {   # wait_for FILE PATTERN SECONDS
    local n=$(( ${3:-15} * 10 ))
    for _ in $(seq 1 "$n"); do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

mkdir -p "$T/src/sub"
head -c $((3 * 1024 * 1024 + 77)) /dev/urandom > "$T/src/w.bin"
head -c 4096 /dev/urandom > "$T/src/sub/tok.bin"
printf '{"model_type":"olmoe"}\n' > "$T/src/config.json"

./lumabri key --out "$T/swarm" > /dev/null
./lumabri key --out "$T/other" > /dev/null      # a different operator entirely
PUB=$(cat "$T/swarm.pub")

./tracker --port 7580 --pubkey "$T/swarm.pub" > "$T/tracker.log" 2>&1 & PIDS+=($!)
wait_port 7580
./maintainer --root "$T/src" --port 7581 --tracker 127.0.0.1:7580 --name origin \
             --model-name fx --key "$T/swarm.key" > "$T/origin.log" 2>&1 & ORIGIN=$!; PIDS+=($!)
wait_port 7581
wait_for "$T/tracker.log" "+ origin" 15 || { cat "$T/tracker.log"; exit 1; }

echo "· 1) the donor verifies what it pulls"
./lumabri serve --model "$T/donor" --join 127.0.0.1:7580 --model-name fx \
    --donate 1 --pubkey "$T/swarm.pub" --port 7582 > "$T/donor.log" 2>&1 & DONOR=$!; PIDS+=($!)
wait_for "$T/donor.log" "assigned slice" 30 || { cat "$T/donor.log"; exit 1; }
if grep -q "pulling unverified" "$T/donor.log"; then
    echo "   the donor pulled bytes it never checked"
    grep "unverified" "$T/donor.log"; exit 1
fi
for f in w.bin sub/tok.bin config.json; do
    cmp "$T/src/$f" "$T/donor/$f" || { echo "   $f differs"; exit 1; }
done
echo "   ✓ 3 files pulled, every chunk checked against the signed truth"

echo "· 2) the tracker accepts what the donor announces"
wait_for "$T/tracker.log" "+ peer-" 15 || { cat "$T/tracker.log"; exit 1; }
if grep -q "REJECTED: peer-" "$T/tracker.log"; then
    echo "   the donor announced bytes the tracker would not take:"
    grep "REJECTED" "$T/tracker.log"; exit 1
fi
NF=$(grep -o "+ peer-[0-9]* @ [^ ]* (fx, [0-9]* files)" "$T/tracker.log" |
     tail -1 | grep -o "[0-9]* files" | cut -d' ' -f1)
[ "${NF:-0}" -ge 3 ] || {
    echo "   the donor registered with ${NF:-0} files — it donated nothing"
    grep "peer-" "$T/tracker.log"; exit 1; }
echo "   ✓ registered with $NF files: the origin's signature travelled with them"

echo "· 3) the donated copy alone serves a chatter that checks the signature"
kill "$ORIGIN" 2>/dev/null || true
wait "$ORIGIN" 2>/dev/null || true
sleep 1
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v" LUMABRI_CACHE="$T/c" \
    LUMABRI_TRACKER=127.0.0.1:7580 LUMABRI_MODEL=fx LUMABRI_BLOCK_MIB=1 \
    LUMABRI_PUBKEY="$PUB" ./test_shim "$T/v" "$T/src" 2> "$T/shim.err"
grep -q "signed by the operator key" "$T/shim.err" || {
    echo "   the chatter never saw a valid signature"; cat "$T/shim.err"; exit 1; }
echo "   ✓ origin gone, bytes identical, signature still verified"

echo "· 4) the signature survives a restart"
kill "$DONOR" 2>/dev/null || true
wait "$DONOR" 2>/dev/null || true
: > "$T/tracker.log"
./lumabri serve --model "$T/donor" --join 127.0.0.1:7580 --model-name fx \
    --donate 1 --pubkey "$T/swarm.pub" --port 7584 > "$T/donor2.log" 2>&1 & PIDS+=($!)
wait_for "$T/tracker.log" "+ peer-" 30 || { cat "$T/donor2.log"; exit 1; }
sleep 1
if grep -q "REJECTED: peer-" "$T/tracker.log"; then
    echo "   after a restart the donor announced unsigned bytes:"
    grep "REJECTED" "$T/tracker.log"; exit 1
fi
echo "   ✓ it re-registered signed, without re-downloading or re-hashing"

echo "· 5) the wrong operator key holds nothing"
./lumabri serve --model "$T/wrong" --join 127.0.0.1:7580 --model-name fx \
    --donate 1 --pubkey "$T/other.pub" --port 7586 > "$T/wrong.log" 2>&1 & PIDS+=($!)
wait_for "$T/wrong.log" "assigned slice" 30 || { cat "$T/wrong.log"; exit 1; }
grep -q "does not match the operator key" "$T/wrong.log" || {
    echo "   a donor with the wrong key did not notice"; cat "$T/wrong.log"; exit 1; }
grep -q "assigned slice: 0/" "$T/wrong.log" || {
    echo "   a donor with the wrong key held something anyway"
    grep "assigned slice" "$T/wrong.log"; exit 1; }
[ -z "$(ls -A "$T/wrong" 2>/dev/null | grep -v lumabri_hashes || true)" ] || {
    echo "   it wrote files it could not verify"; ls -A "$T/wrong"; exit 1; }
echo "   ✓ refused every file, kept none: the check is real"

echo "· 6) a correct manifest is not a promise about the bytes"
# The adversary worth testing is not the one who lies in the index — the
# tracker already refuses that. It is the peer whose announcement is
# perfectly valid, signature and all, and who then serves something else.
# Its own hashes convict it.
./tracker --port 7587 --pubkey "$T/swarm.pub" > "$T/tracker2.log" 2>&1 & PIDS+=($!)
wait_port 7587
LUMABRI_CORRUPT_PPM=1000000 \
    ./maintainer --root "$T/src" --port 7588 --tracker 127.0.0.1:7587 \
                 --name liar --model-name fy --key "$T/swarm.key" \
                 > "$T/liar.log" 2>&1 & PIDS+=($!)
wait_port 7588
wait_for "$T/tracker2.log" "+ liar" 15 || { cat "$T/tracker2.log"; exit 1; }
./maintainer --root "$T/victim" --port 7589 --tracker 127.0.0.1:7587 \
             --name victim --model-name fy --donate 1 --pubkey "$T/swarm.pub" \
             > "$T/victim.log" 2>&1 || true
grep -q "served corrupt bytes" "$T/victim.log" || {
    echo "   the donor accepted bytes that did not match their own hashes"
    cat "$T/victim.log"; exit 1; }
grep -q "assigned slice: 0/" "$T/victim.log" || {
    echo "   it kept something from a peer it had caught lying"
    grep "assigned slice" "$T/victim.log"; exit 1; }
echo "   ✓ caught the lie, pulled nothing, and said which peer told it"

echo "LUMABRI SIGNED DONOR TEST: PASS"
