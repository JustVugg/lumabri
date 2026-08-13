#!/usr/bin/env bash
# lumabri — a name belongs to the key that first claimed it.
#
# The tracker used to locate a peer's slot by name alone, so anyone who could
# reach it could register under an honest peer's name and take over its slot,
# redirecting or evicting it. Each peer now signs a per-connection nonce with
# its own Ed25519 key; the tracker binds the name to that key on first use and
# refuses a different key under the same name.
#
#   1) an honest maintainer registers and is reachable at its address;
#   2) an impostor with the SAME name but a different key is rejected, and the
#      honest peer's address is untouched;
#   3) the honest peer, restarted with the same key, reclaims its own name;
#   4) a REGISTER with no signature at all is refused.
set -euo pipefail
cd "$(dirname "$0")"

make -s tracker maintainer

T=$(mktemp -d /tmp/lumabri-peerid.XXXXXX)
export HOME="$T"
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
head -c $((1024 * 1024)) /dev/urandom > "$T/src/w.bin"
printf '{"model_type":"olmoe"}\n' > "$T/src/config.json"

# is address ADDR listed among the holders of MODEL at the tracker?
cat > "$T/probe.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    LmbBuf b = {0}; LmbMsg m = {0};
    lmb_buf_str(&b, argv[2]);
    if (lmb_request(argv[1], LMB_PLACEMENT, b.p, (uint32_t)b.len, &m) ||
        m.op != LMB_PLACEMENT_R) return 2;
    LmbCur c = { m.body, m.body_len, 0 }; uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) return 2;
    for (uint32_t i = 0; i < n; i++) {
        char path[LMB_PATH_MAX], peer[64]; uint64_t size; uint16_t np;
        if (lmb_cur_str(&c, path, sizeof path) || lmb_cur_u64(&c, &size) ||
            lmb_cur_u16(&c, &np)) return 2;
        for (uint16_t p = 0; p < np; p++) {
            if (lmb_cur_str(&c, peer, sizeof peer)) return 2;
            if (!strcmp(peer, argv[3])) return 0;   /* present */
        }
    }
    return 1;                                       /* absent */
}
EOF
cc -O2 -w -I. "$T/probe.c" -o "$T/probe" -lpthread

# a REGISTER with no identity block, to prove the signature is mandatory
cat > "$T/noauth.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    int fd = lmb_connect(argv[1]);
    if (fd < 0) return 2;
    LmbBuf b = {0};
    lmb_buf_str(&b, "origin"); lmb_buf_str(&b, "127.0.0.1:1");
    lmb_buf_str(&b, "fx"); lmb_buf_u64(&b, 0);
    lmb_buf_u64(&b, 0); lmb_buf_u64(&b, 0); lmb_buf_u32(&b, 0);
    LmbMsg m = {0};
    int rc = lmb_send(fd, LMB_REGISTER, b.p, (uint32_t)b.len, NULL, 0) ||
             lmb_recv(fd, &m);
    int refused = !rc && m.op == LMB_ERR;
    lmb_msg_free(&m); close(fd);
    return refused ? 0 : 1;
}
EOF
cc -O2 -w -I. "$T/noauth.c" -o "$T/noauth" -lpthread

./tracker --port 7640 > "$T/tracker.log" 2>&1 & TRACKER=$!; PIDS+=($TRACKER)
wait_port 7640

run_origin() {   # run_origin PORT KEYFILE  -> background pid in $LASTPID
    env LUMABRI_PEER_KEY="$2" ./maintainer --root "$T/src" --port "$1" \
        --tracker 127.0.0.1:7640 --name origin --model-name fx \
        --advertise "127.0.0.1:$1" > "$T/origin-$1.log" 2>&1 &
    LASTPID=$!
}

wait_placed() {  # wait until ADDR is a holder of fx
    for _ in $(seq 1 100); do
        "$T/probe" 127.0.0.1:7640 fx "$1" && return 0
        sleep 0.1
    done
    return 1
}

echo "· 1) an honest maintainer registers under its name"
run_origin 7641 "$T/keyA"; PIDS+=($LASTPID); HONEST=$LASTPID
wait_port 7641
wait_placed 127.0.0.1:7641 || { echo "   honest origin never appeared"; cat "$T/origin-7641.log"; exit 1; }
echo "   ✓ origin @ 127.0.0.1:7641"

echo "· 2) an impostor with the same name but a different key is refused"
run_origin 7642 "$T/keyB"; PIDS+=($LASTPID)
wait_port 7642
sleep 2                                   # give it heartbeats to try
for _ in $(seq 1 50); do
    grep -q "already held by another key" "$T/tracker.log" && break
    sleep 0.1
done
grep -q "already held by another key" "$T/tracker.log" || {
    echo "   the tracker did not reject the impostor"; cat "$T/tracker.log"; exit 1; }
if "$T/probe" 127.0.0.1:7640 fx 127.0.0.1:7642; then
    echo "   the impostor's address took over the name"; exit 1; fi
"$T/probe" 127.0.0.1:7640 fx 127.0.0.1:7641 || {
    echo "   the honest peer's address was displaced"; exit 1; }
echo "   ✓ rejected; origin still @ 127.0.0.1:7641"

echo "· 3) the honest peer restarts with the same key and reclaims its name"
kill "$HONEST" 2>/dev/null || true
wait "$HONEST" 2>/dev/null || true
run_origin 7643 "$T/keyA"; PIDS+=($LASTPID)   # same keyA, new port
wait_port 7643
wait_placed 127.0.0.1:7643 || {
    echo "   the honest peer could not reclaim its own name"; cat "$T/origin-7643.log"; exit 1; }
echo "   ✓ reclaimed with the same key"

echo "· 4) a REGISTER with no signature is refused"
"$T/noauth" 127.0.0.1:7640 || { echo "   an unsigned REGISTER was accepted"; exit 1; }
echo "   ✓ unauthenticated registration refused"

echo "· 5) the name binding survives a tracker restart"
kill "$TRACKER" 2>/dev/null || true
wait "$TRACKER" 2>/dev/null || true
./tracker --port 7640 > "$T/tracker-restart.log" 2>&1 & TRACKER2=$!; PIDS+=($TRACKER2)
wait_port 7640
# keyB is already retrying in the background; it must remain rejected while
# the honest keyA control connection reconnects and reclaims the placement.
for _ in $(seq 1 100); do
    grep -q "already held by another key" "$T/tracker-restart.log" &&
    "$T/probe" 127.0.0.1:7640 fx 127.0.0.1:7643 && break
    sleep 0.1
done
grep -q "already held by another key" "$T/tracker-restart.log" || {
    echo "   restarted tracker forgot the bound key"; cat "$T/tracker-restart.log"; exit 1; }
"$T/probe" 127.0.0.1:7640 fx 127.0.0.1:7643 || {
    echo "   honest key could not reclaim after tracker restart"; exit 1; }
echo "   ✓ persisted binding rejected keyB and admitted keyA after restart"

echo "LUMABRI PEER IDENTITY TEST: PASS"
