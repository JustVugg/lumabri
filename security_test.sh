#!/usr/bin/env bash
# lumabri — the attacks that come from the network naming things.
#
# Everything else in this project asks "are the bytes right". This asks the
# question before that one: whose disk gets written, and where.
#
# A chatter builds its mirror from names it is TOLD — a tracker's placement,
# a peer's manifest, a slice assignment. Each of those names is joined onto a
# local directory and opened with O_CREAT before a single byte is verified,
# so a name like `../../.bashrc` is a file creation and a truncation on the
# victim's disk, chosen by the attacker. Signatures do not help: the mirror
# file exists before any hash is fetched, and the attacker picks the name.
#
#   1) a hostile PEER offers an escaping name in its manifest
#   2) a hostile TRACKER offers one in its placement
#   3) the TRACKER refuses to carry one at all, so it never spreads
#
# In every case the victim's file must not be touched.
set -euo pipefail
cd "$(dirname "$0")"

make -s all
T=$(mktemp -d /tmp/lumabri-sec.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

CANARY="$T/precious.txt"
echo "do not touch me" > "$CANARY"
CANARY_SUM=$(sha256sum "$CANARY" | cut -d' ' -f1)

# A peer that answers MANIFEST with a name climbing out of the mirror. It
# speaks just enough of the wire protocol to lie once.
cat > "$T/evil_peer.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    int port = atoi(argv[1]);
    const char *evil = argv[2];
    int lfd = lmb_listen(port);
    if (lfd < 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        LmbMsg m;
        while (lmb_recv(fd, &m) == 0) {
            if (m.op == LMB_PING) lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            else if (m.op == LMB_MANIFEST) {
                LmbBuf b = {0};
                lmb_buf_u32(&b, 1);
                lmb_buf_str(&b, evil);
                lmb_buf_u64(&b, 4096);
                lmb_send(fd, LMB_MANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
                free(b.p);
            } else lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            lmb_msg_free(&m);
        }
        close(fd);
    }
}
EOF
cc -O2 -w -I. "$T/evil_peer.c" -o "$T/evil_peer" -lpthread

# The escaping name, relative to <cache>/data/
ESCAPE="../../../../../../..$CANARY"

echo "· 1) a peer's manifest names a file outside the mirror"
"$T/evil_peer" 7551 "$ESCAPE" & PIDS+=($!)
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/7551) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.1
done
set +e
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v" LUMABRI_CACHE="$T/c" \
    LUMABRI_PEERS=127.0.0.1:7551 ./test_shim "$T/v" "$T" > "$T/p.out" 2> "$T/p.err"
set -e
grep -q "unsafe file name" "$T/p.err" || {
    echo "   the shim accepted an escaping name from a peer"; cat "$T/p.err"; exit 1; }
NOW=$(sha256sum "$CANARY" | cut -d' ' -f1)
[ "$NOW" = "$CANARY_SUM" ] || { echo "   THE CANARY WAS MODIFIED — path escape works"; exit 1; }
echo "   ✓ refused, and the file outside the mirror is untouched"

echo "· 2) a tracker's placement names a file outside the mirror"
cat > "$T/evil_tracker.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    int port = atoi(argv[1]);
    const char *evil = argv[2];
    int lfd = lmb_listen(port);
    if (lfd < 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        LmbMsg m;
        while (lmb_recv(fd, &m) == 0) {
            if (m.op == LMB_PLACEMENT) {
                LmbBuf b = {0};
                lmb_buf_u32(&b, 1);          /* one file */
                lmb_buf_str(&b, evil);
                lmb_buf_u64(&b, 4096);
                lmb_buf_u16(&b, 0);          /* held by nobody: names only */
                lmb_send(fd, LMB_PLACEMENT_R, b.p, (uint32_t)b.len, NULL, 0);
                free(b.p);
            } else lmb_send(fd, LMB_OK, NULL, 0, NULL, 0);
            lmb_msg_free(&m);
        }
        close(fd);
    }
}
EOF
cc -O2 -w -I. "$T/evil_tracker.c" -o "$T/evil_tracker" -lpthread
"$T/evil_tracker" 7552 "$ESCAPE" & PIDS+=($!)
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/7552) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.1
done
set +e
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v2" LUMABRI_CACHE="$T/c2" \
    LUMABRI_TRACKER=127.0.0.1:7552 ./test_shim "$T/v2" "$T" > "$T/t.out" 2> "$T/t.err"
set -e
grep -q "unsafe file name" "$T/t.err" || {
    echo "   the shim accepted an escaping name from the tracker"; cat "$T/t.err"; exit 1; }
NOW=$(sha256sum "$CANARY" | cut -d' ' -f1)
[ "$NOW" = "$CANARY_SUM" ] || { echo "   THE CANARY WAS MODIFIED — path escape works"; exit 1; }
echo "   ✓ refused, and the file outside the mirror is untouched"

echo "· 3) the real tracker refuses to carry such a name at all"
./tracker --port 7553 > "$T/tracker.log" 2>&1 & PIDS+=($!)
sleep 0.4
cat > "$T/evil_reg.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    int fd = lmb_connect(argv[1]);
    if (fd < 0) return 1;
    LmbBuf b = {0};
    lmb_buf_str(&b, "liar");
    lmb_buf_str(&b, "127.0.0.1:9999");
    lmb_buf_str(&b, "model");
    lmb_buf_u64(&b, 4096);      /* held */
    lmb_buf_u64(&b, 0);         /* served bytes */
    lmb_buf_u64(&b, 0);         /* served reads */
    lmb_buf_u32(&b, 1);
    lmb_buf_str(&b, argv[2]);
    lmb_buf_u64(&b, 4096);
    lmb_send(fd, LMB_REGISTER, b.p, (uint32_t)b.len, NULL, 0);
    LmbMsg m;
    if (lmb_recv(fd, &m)) return 2;
    return m.op == LMB_ERR ? 0 : 3;      /* 0 = refused, as it must be */
}
EOF
cc -O2 -w -I. "$T/evil_reg.c" -o "$T/evil_reg" -lpthread
set +e
"$T/evil_reg" 127.0.0.1:7553 "$ESCAPE"
RC=$?
set -e
[ "$RC" -eq 0 ] || { echo "   the tracker ACCEPTED an unsafe file name (rc=$RC)"; exit 1; }
grep -q "REJECTED" "$T/tracker.log" || { echo "   the tracker said nothing"; cat "$T/tracker.log"; exit 1; }
echo "   ✓ refused at the index, so it never reaches a client"

echo "LUMABRI SECURITY TEST: PASS"
