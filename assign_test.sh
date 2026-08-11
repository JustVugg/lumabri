#!/usr/bin/env bash
# lumabri — "the server decides", for compute.
#
# A donor of disk has always been told what to hold: it offers a byte budget
# and the tracker answers with the least-replicated files. A donor of COMPUTE
# had to be told by hand — `--stride 3:1` — which means knowing how many other
# donors exist and which index is still free. That is coordination, and a
# swarm is supposed to remove coordination.
#
# With --hold N a node states only what it knows about itself: how many
# experts it can carry. Three of them start one after another, none aware of
# the others, and this checks that:
#
#   1) they end up DISJOINT — nobody duplicates what is already covered;
#   2) together they COVER the whole expert set;
#   3) a restart keeps what it had, so nothing is re-downloaded.
set -euo pipefail
cd "$(dirname "$0")"

MODEL="${MODEL:-$PWD/tiny_olmoe}"
PORT="${PORT:-7560}"
ENGINE="${ENGINE:-../moe-stream/c}"

make -s tracker expert_node ENGINE="$ENGINE"
[ -f "$MODEL/config.json" ] || make -s fixture

T=$(mktemp -d /tmp/lumabri-assign.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

TOTAL=$(python3 -c "
import json; c=json.load(open('$MODEL/config.json'))
print(c['num_hidden_layers']*c['num_experts'])")
THIRD=$(( (TOTAL + 2) / 3 ))
echo "· $TOTAL esperti in totale; ognuno ne chiede $THIRD, nessuno sa degli altri"

./tracker --port "$PORT" > "$T/tracker.log" 2>&1 & PIDS+=($!)
for _ in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.1
done

# each node: same command, only the port and name differ. No stride, no index.
for i in 0 1 2; do
    p=$((PORT + 10 + i))
    OMP_NUM_THREADS=1 ./expert_node --model "$MODEL" --port "$p" \
        --tracker "127.0.0.1:$PORT" --advertise "127.0.0.1:$p" \
        --model-name tiny_olmoe --name "node-$i" \
        --hold "$THIRD" --cache 8 > "$T/n$i.log" 2>&1 & PIDS+=($!)
    for _ in $(seq 1 300); do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; break; }
        sleep 0.2
    done
    sleep 2                     # let its heartbeat reach the tracker
    grep -h "assigned" "$T/n$i.log" || true
done

# ask each node what it actually holds, straight from its EMANIFEST
cat > "$T/ask.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    LmbMsg m = {0};
    if (lmb_request(argv[1], LMB_EMANIFEST, NULL, 0, &m) || m.op != LMB_EMANIFEST_R)
        return 1;
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t magic = 0, bits = 0, have_id = 0, has_sig = 0, n = 0, hidden = 0;
    uint8_t root[32], sig[64];
    char engine[64], profile[LMB_BUILD_PROFILE_MAX], model[64];
    if (lmb_cur_u32(&c, &magic) || magic != LMB_EXPERT_MANIFEST_MAGIC ||
        lmb_cur_str(&c, engine, sizeof engine) ||
        lmb_cur_str(&c, profile, sizeof profile) ||
        lmb_cur_str(&c, model, sizeof model) ||
        lmb_cur_u32(&c, &bits) || lmb_cur_u32(&c, &have_id) || have_id > 1)
        return 2;
    if (have_id && (lmb_cur_bytes(&c, root, sizeof root) ||
                    lmb_cur_u32(&c, &has_sig) || has_sig > 1 ||
                    (has_sig && lmb_cur_bytes(&c, sig, sizeof sig))))
        return 2;
    if (lmb_cur_u32(&c, &n)) return 2;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l, e;
        if (lmb_cur_u32(&c, &l) || lmb_cur_u32(&c, &e)) return 2;
        printf("%u:%u\n", l, e);
    }
    return lmb_cur_u32(&c, &hidden) || c.off != c.len ? 2 : 0;
}
EOF
cc -O2 -w -I. "$T/ask.c" -o "$T/ask" -lpthread
for i in 0 1 2; do "$T/ask" "127.0.0.1:$((PORT + 10 + i))" | sort > "$T/held$i"; done

echo
# 3 nodes asking for ceil(TOTAL/3) each ask for slightly MORE than exists, and
# the surplus is not waste: once every expert is covered, further capacity
# becomes redundancy — the rarest get a second holder. So the expected overlap
# is exactly the surplus, and anything above it means two nodes chose the same
# thing while something was still uncovered.
echo "· 1) nessuna sovrapposizione finche' c'e' qualcosa di scoperto"
DUP=$(cat "$T/held0" "$T/held1" "$T/held2" | sort | uniq -d | wc -l)
WANT=$(( 3 * THIRD - TOTAL )); [ "$WANT" -lt 0 ] && WANT=0
for i in 0 1 2; do printf "   node-%d: %s esperti\n" "$i" "$(wc -l < "$T/held$i")"; done
echo "   duplicati: $DUP (attesi $WANT: capacita' in eccesso -> repliche)"
[ "$DUP" -eq "$WANT" ] || {
    echo "   sovrapposizione oltre il surplus: l'assegnazione non e' rarest-first"; exit 1; }
echo "   ✓ ogni nodo ha preso solo cio' che nessuno copriva"

echo "· 2) coprono tutto"
COV=$(cat "$T/held0" "$T/held1" "$T/held2" | sort -u | wc -l)
echo "   coperti $COV / $TOTAL"
[ "$COV" -eq "$TOTAL" ] || { echo "   copertura incompleta"; exit 1; }
echo "   ✓ l'intero set, senza che nessuno si coordinasse"

echo "· 3) un riavvio non cambia idea"
kill "${PIDS[3]}" 2>/dev/null || true      # node-2
sleep 1
OMP_NUM_THREADS=1 ./expert_node --model "$MODEL" --port "$((PORT + 12))" \
    --tracker "127.0.0.1:$PORT" --advertise "127.0.0.1:$((PORT + 12))" \
    --model-name tiny_olmoe --name "node-2" \
    --hold "$THIRD" --cache 8 > "$T/n2b.log" 2>&1 & PIDS+=($!)
for _ in $(seq 1 300); do
    (exec 3<>/dev/tcp/127.0.0.1/$((PORT + 12))) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.2
done
"$T/ask" "127.0.0.1:$((PORT + 12))" | sort > "$T/held2b"
if diff -q "$T/held2" "$T/held2b" > /dev/null; then
    echo "   ✓ stessa fetta dopo il riavvio: niente da riscaricare"
else
    echo "   la fetta e' cambiata dopo il riavvio ($(comm -13 "$T/held2" "$T/held2b" | wc -l) nuovi)"
    exit 1
fi

echo "LUMABRI ASSIGN TEST: PASS"
