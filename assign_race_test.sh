#!/usr/bin/env bash
# The race behind #52: a node asks EASSIGN, then spends minutes loading its
# slice before the first EREG makes the holding visible. A second node asking
# in that window used to receive the SAME "least-covered" slice — half the
# model assigned twice, the other half to nobody. The tracker now remembers
# what it promised. Two back-to-back asks, no EREG in between, must come back
# disjoint and covering; a re-ask under the first name must keep its slice.
set -euo pipefail
cd "$(dirname "$0")"

make -s tracker

T=$(mktemp -d /tmp/lumabri-assignrace.XXXXXX)
export LUMABRI_PEER_BINDINGS="$T/peer-bindings"
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

cat > "$T/ask.c" <<'EOF'
/* one EASSIGN, prints the picked cells (layer*nexp+expert), one per line */
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    if (argc != 4) return 2;                 /* TRACKER NAME CAPACITY */
    uint32_t slots = 4, nexp = 8, cap = (uint32_t)atoi(argv[3]);
    uint8_t routed[4] = { 1, 1, 1, 1 };
    LmbBuf b = {0};
    lmb_buf_str(&b, "race-model");
    lmb_buf_str(&b, argv[2]);
    lmb_buf_u32(&b, slots); lmb_buf_u32(&b, nexp); lmb_buf_u32(&b, cap);
    lmb_buf_u32(&b, slots); lmb_buf_bytes(&b, routed, slots);
    LmbMsg m = {0};
    if (lmb_request(argv[1], LMB_EASSIGN, b.p, (uint32_t)b.len, &m) ||
        m.op != LMB_EASSIGN_R) return 1;
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t n = 0;
    if (lmb_cur_u32(&c, &n)) return 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l, e;
        if (lmb_cur_u32(&c, &l) || lmb_cur_u32(&c, &e)) return 1;
        printf("%u\n", l * nexp + e);
    }
    return 0;
}
EOF
cc -O2 -Wall -I. -pthread "$T/ask.c" -o "$T/ask"

./tracker --port 7641 > "$T/tracker.log" 2>&1 & PIDS+=($!)
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/7641) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.1
done

# 32 cells in all; each asks for 16, back to back, nobody EREG-registered
"$T/ask" 127.0.0.1:7641 node-a 16 | sort > "$T/a"
"$T/ask" 127.0.0.1:7641 node-b 16 | sort > "$T/b"

na=$(wc -l < "$T/a"); nb=$(wc -l < "$T/b")
[ "$na" -eq 16 ] && [ "$nb" -eq 16 ] || {
    echo "✗ expected 16+16 experts, got $na+$nb"; exit 1; }
dup=$(comm -12 "$T/a" "$T/b" | wc -l)
[ "$dup" -eq 0 ] || {
    echo "✗ the two slices overlap on $dup experts — the promise was forgotten"
    exit 1; }
tot=$(sort "$T/a" "$T/b" | uniq | wc -l)
[ "$tot" -eq 32 ] || { echo "✗ union covers $tot of 32"; exit 1; }
echo "✓ two back-to-back asks: disjoint halves, full cover, no EREG needed"

# a re-ask mid-load keeps the promised slice — no churn, no re-download
"$T/ask" 127.0.0.1:7641 node-a 16 | sort > "$T/a2"
cmp -s "$T/a" "$T/a2" || { echo "✗ re-ask changed node-a's slice"; exit 1; }
echo "✓ re-ask under the same name keeps its promised slice"
echo "ASSIGN RACE: PASS"
