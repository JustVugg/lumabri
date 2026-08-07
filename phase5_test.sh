#!/usr/bin/env bash
# lumabri phase 5 — the open swarm: lying peers must be caught. Three claims:
#
#   1) LYING BYTES    a maintainer that serves corrupt bytes (correct
#                     manifest, wrong data — the exact adversarial lie) is
#                     rejected block by block; the mirror is built from the
#                     honest replica and stays byte-identical.
#   2) POISON INDEX   a maintainer announcing different bytes for a file the
#                     swarm already knows is stripped at registration: its
#                     lie never enters a placement.
#   3) LYING EXPERT   an executor returning wrong activations is caught by
#                     the spot-check (a second replica must agree to the
#                     byte) and the run refuses to continue.
#
# The corrupt peers use LUMABRI_CORRUPT_PPM, a test-only switch that makes a
# peer serve what an adversary would: everything honest except the bytes.
set -euo pipefail
cd "$(dirname "$0")"

make -s all

T=$(mktemp -d /tmp/lumabri-phase5.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

mkdir -p "$T/src"
head -c $((12 * 1024 * 1024)) /dev/urandom > "$T/src/w.bin"
head -c 777 /dev/urandom > "$T/src/config.json"

wait_port() {
    for _ in $(seq 1 200); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.2
    done
    return 1
}

echo "· 1) lying bytes: every corrupt block rejected, mirror byte-identical"
./tracker --port 7450 & PIDS+=($!)
sleep 0.3
# the liar registers FIRST and is nearest — the worst case: it computes
# honest hashes from the true file, then corrupts every read it serves
LUMABRI_CORRUPT_PPM=1000000 \
./maintainer --root "$T/src" --port 7451 --tracker 127.0.0.1:7450 --name liar \
    > "$T/liar.log" 2>&1 & PIDS+=($!)
./maintainer --root "$T/src" --port 7452 --tracker 127.0.0.1:7450 --name honest \
    > "$T/honest.log" 2>&1 & PIDS+=($!)
wait_port 7451; wait_port 7452
sleep 0.5
env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_VROOT="$T/v1" LUMABRI_CACHE="$T/c1" \
    LUMABRI_TRACKER=127.0.0.1:7450 LUMABRI_BLOCK_MIB=1 LUMABRI_REQUIRE_HASH=1 \
    ./test_shim "$T/v1" "$T/src" 2> "$T/shim.err"
REJ=$(grep -c "CORRUPT" "$T/shim.err" || true)
echo "   corrupt blocks rejected: $REJ"
[ "$REJ" -gt 0 ] || { echo "   LYING BYTES UNPROVEN: the liar was never used"; exit 1; }
echo "   ✓ every lie rejected, every byte identical (REQUIRE_HASH strict mode)"

echo "· 2) poison index: different bytes for a known file die at registration"
mkdir -p "$T/evil"
head -c $((12 * 1024 * 1024)) /dev/urandom > "$T/evil/w.bin"   # different bytes!
head -c 777 /dev/urandom > "$T/evil/config.json"
./maintainer --root "$T/evil" --port 7453 --tracker 127.0.0.1:7450 \
    --name poisoner --model-name src > "$T/evil.log" 2>&1 & PIDS+=($!)
wait_port 7453
sleep 1.5
# assert on the tracker's own answer: the poisoner must hold zero placeable
# files, so its address can appear in no placement at all
python3 - <<'EOF'
import socket, struct, sys
MAGIC = 0x31424D4C
def req(op, body=b""):
    s = socket.create_connection(("127.0.0.1", 7450), 3)
    s.sendall(struct.pack("<IIII", MAGIC, op, len(body), 0) + body)
    pre = b""
    while len(pre) < 16: pre += s.recv(16 - len(pre))
    m, o, bl, pl = struct.unpack("<IIII", pre)
    data = b""
    while len(data) < bl + pl: data += s.recv(bl + pl - len(data))
    s.close(); return o, data
def lstr(t): return struct.pack("<H", len(t)) + t.encode()
op, body = req(9, lstr("src"))            # PLACEMENT for model 'src'
assert op == 10
off = 0
(n,) = struct.unpack_from("<I", body, off); off += 4
bad = 0
for _ in range(n):
    (sl,) = struct.unpack_from("<H", body, off); off += 2
    path = body[off:off+sl].decode(); off += sl
    off += 8
    (np,) = struct.unpack_from("<H", body, off); off += 2
    for _ in range(np):
        (al,) = struct.unpack_from("<H", body, off); off += 2
        addr = body[off:off+al].decode(); off += al
        if addr.endswith(":7453"): bad += 1
if bad: print(f"POISON LEAKED: the poisoner appears in {bad} placements"); sys.exit(1)
print("   placements clean: the poisoner holds nothing the swarm will use")
EOF
echo "   ✓ poison stripped at the index"
kill "${PIDS[@]}" 2>/dev/null || true; wait 2>/dev/null || true; PIDS=()

echo "· 3) lying expert: the spot-check catches a wrong activation"
make -s phase2
MODEL="$PWD/tiny_olmoe"
[ -f "$MODEL/config.json" ] || make -s fixture
LUMABRI_CORRUPT_PPM=1000000 OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 \
    ./expert_node --model "$MODEL" --port 7454 --name liar-exec \
    > "$T/lex.log" 2>&1 & PIDS+=($!)
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 LUMABRI_RTT_US=2000 \
    ./expert_node --model "$MODEL" --port 7455 --name honest-exec \
    > "$T/hex.log" 2>&1 & PIDS+=($!)
wait_port 7454; wait_port 7455
set +e
SNAP="$MODEL" OMP_NUM_THREADS=6 LUMABRI_VERIFY=100 \
    LUMABRI_EXPERTS="127.0.0.1:7454,127.0.0.1:7455" \
    ./olmoe_p2p 16 8 "$MODEL/ref.json" > "$T/spot.out" 2>"$T/spot.err"
RC=$?
set -e
grep -q "INTEGRITY FAILURE" "$T/spot.err" || {
    echo "   LYING EXPERT UNPROVEN: no integrity failure raised"; exit 1; }
[ "$RC" -ne 0 ] || { echo "   LYING EXPERT FAILED: the run continued"; exit 1; }
echo "   ✓ the lie was caught on the first spot-check and the run refused to continue"

echo "LUMABRI PHASE 5: PASS"
