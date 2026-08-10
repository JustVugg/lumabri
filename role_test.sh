#!/usr/bin/env bash
# lumabri — --role means whole words.
#
# The roles were matched by letter, which is shorter and wrong: strchr("chat",
# 'c') is true, so "chat" — the one role that donates nothing — was the role
# that switched compute donation on. Nobody noticed because a donor that
# nobody calls looks exactly like no donor at all, until it is holding your
# CPU during a benchmark.
#
# The observable is the donor's model directory: it is created while the role
# is being parsed, before anything else can fail, so it says what the flag was
# understood to mean without needing an engine or a model.
set -euo pipefail
cd "$(dirname "$0")"

make -s lumabri tracker maintainer

T=$(mktemp -d /tmp/lumabri-role.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

mkdir -p "$T/home" "$T/src"
printf '{"model_type":"olmoe"}\n' > "$T/src/config.json"
head -c 65536 /dev/urandom > "$T/src/w.bin"

./tracker --port 7598 > "$T/tracker.log" 2>&1 & PIDS+=($!)
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/7598) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.1
done
./maintainer --root "$T/src" --port 7599 --tracker 127.0.0.1:7598 \
             --name origin --model-name fx > "$T/origin.log" 2>&1 & PIDS+=($!)
sleep 1.5

# role → does it become a donor?
try() {                       # try ROLE EXPECT_DONOR EXPECT_RC
    rm -rf "$T/home/.lumabri"
    set +e
    HOME="$T/home" timeout 30 ./lumabri chat --tracker 127.0.0.1:7598 \
        --model fx --role "$1" --engines-dir "$T/nonexistent" \
        > "$T/out" 2>&1
    local rc=$?
    set -e
    local donor=no
    [ -d "$T/home/.lumabri/fx/donated" ] && donor=yes
    if [ "$donor" != "$2" ]; then
        echo "   --role $1: donor=$donor, atteso $2"; cat "$T/out"; exit 1
    fi
    if [ -n "${3:-}" ] && [ "$rc" != "$3" ]; then
        echo "   --role $1: rc=$rc, atteso $3"; cat "$T/out"; exit 1
    fi
    printf '   %-14s donatore: %-3s\n' "$1" "$donor"
}

echo "· chat is not compute"
try chat no
try disk yes
try compute yes
try disk,compute yes
try all yes
echo "   ✓ ogni parola vale per se stessa"

echo "· a typo is refused, not silently demoted to chat"
try bogus no 2
grep -q 'non conosco "bogus"' "$T/out" || {
    echo "   nessun messaggio utile per un ruolo sconosciuto"; cat "$T/out"; exit 1; }
# and the half-right case: accepting "chat,compute-ish" because "chat" is in
# there would drop exactly the half the user meant to add
try chat,bogus no 2
grep -q 'non conosco "bogus"' "$T/out" || {
    echo "   un ruolo valido ha coperto uno sbagliato"; cat "$T/out"; exit 1; }
try disk,compute,nope no 2
echo "   ✓ chi sbaglia a scrivere lo sa subito, anche se ha ragione a meta'"

echo "LUMABRI ROLE TEST: PASS"
