#!/usr/bin/env bash
# The share adapts to the swarm. Three nodes join one after another, each
# with capacity for EVERY expert of tiny_olmoe — so for a moment every
# expert has three copies. The elastic rebalance must then shrink the
# surplus: at most EASSIGN_KEEP_REPLICAS (2) copies per expert, full
# coverage preserved, and the release decided deterministically by the
# tracker so no two nodes ever drop the same expert at the same time.
set -euo pipefail
cd "$(dirname "$0")"

ENGINE="${ENGINE:-../colibri/c}"
MODEL="${MODEL:-$PWD/tiny_olmoe}"
PORT="${PORT:-7661}"

make -s tracker expert_node ENGINE="$ENGINE"
[ -f "$MODEL/config.json" ] || make -s fixture

T=$(mktemp -d /tmp/lumabri-elastic.XXXXXX)
export LUMABRI_PEER_BINDINGS="$T/peer-bindings"
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 300); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    return 1
}

TOTAL=$(python3 -c "
import json; c=json.load(open('$MODEL/config.json'))
print(c['num_hidden_layers']*c['num_experts'])")

cat > "$T/ask.c" <<'EOF'
#include "lumabri_proto.h"
int main(int argc, char **argv) {
    LmbMsg m = {0};
    if (lmb_request(argv[1], LMB_EMANIFEST, NULL, 0, &m) || m.op != LMB_EMANIFEST_R)
        return 1;
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t magic = 0, bits = 0, have_id = 0, has_sig = 0, n = 0;
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
    return 0;
}
EOF
cc -O2 -w -I. "$T/ask.c" -o "$T/ask" -lpthread

./tracker --port "$PORT" > "$T/tracker.log" 2>&1 & PIDS+=($!)
wait_port "$PORT"

echo "· 3 nodi, ognuno con capacità per TUTTI i $TOTAL esperti (surplus voluto)"
for i in 0 1 2; do
    p=$((PORT + 10 + i))
    OMP_NUM_THREADS=1 LUMABRI_REBALANCE_S=10 \
    ./expert_node --model "$MODEL" --port "$p" --tracker "127.0.0.1:$PORT" \
        --advertise "127.0.0.1:$p" --model-name tiny_olmoe --name "el-$i" \
        --hold "$TOTAL" --cache 16 > "$T/n$i.log" 2>&1 & PIDS+=($!)
    wait_port "$p"
    for _ in $(seq 1 100); do
        grep -q "+ expert el-$i" "$T/tracker.log" && break; sleep 0.2
    done
done
for i in 0 1 2; do "$T/ask" "127.0.0.1:$((PORT+10+i))" | sort > "$T/before$i"; done
B=$(cat "$T/before0" "$T/before1" "$T/before2" | wc -l)
echo "  repliche totali all'ingresso: $B"

echo "· aspetto i giri di rebalance (LUMABRI_REBALANCE_S=10)…"
for _ in $(seq 1 12); do
    sleep 5
    for i in 0 1 2; do "$T/ask" "127.0.0.1:$((PORT+10+i))" | sort > "$T/after$i"; done
    A=$(cat "$T/after0" "$T/after1" "$T/after2" | wc -l)
    [ "$A" -le $((2 * TOTAL)) ] && break
done

A=$(cat "$T/after0" "$T/after1" "$T/after2" | wc -l)
COVER=$(cat "$T/after0" "$T/after1" "$T/after2" | sort -u | wc -l)
OVER=$(cat "$T/after0" "$T/after1" "$T/after2" | sort | uniq -c | awk '$1>2' | wc -l)
for i in 0 1 2; do printf "  el-%d: %s esperti\n" "$i" "$(wc -l < "$T/after$i")"; done
echo "  repliche totali dopo: $A (tetto $((2*TOTAL))) · copertura $COVER/$TOTAL · celle oltre 2 repliche: $OVER"

[ "$A" -lt "$B" ]            || { echo "✗ il surplus non si è mai sgonfiato"; exit 1; }
[ "$A" -le $((2 * TOTAL)) ]  || { echo "✗ più di 2 repliche di diritto per esperto"; exit 1; }
[ "$COVER" -eq "$TOTAL" ]    || { echo "✗ il rilascio ha aperto buchi di copertura"; exit 1; }
[ "$OVER" -eq 0 ]            || { echo "✗ $OVER celle ancora sopra le 2 repliche"; exit 1; }
grep -h "rebalance:" "$T"/n*.log | head -3
echo "✓ ELASTICO — il surplus si è sgonfiato, la copertura è rimasta intera"
