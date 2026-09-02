#!/usr/bin/env bash
# The bf16 wire dialect against a real OLMoE node: same bytes back.
set -euo pipefail
cd "$(dirname "$0")"
make -s expert_node test_exec2
MODEL="${MODEL:-$PWD/tiny_olmoe}"
[ -f "$MODEL/config.json" ] || make -s fixture
T=$(mktemp -d /tmp/lumabri-exec2.XXXXXX)
OMP_NUM_THREADS=2 COLI_NO_OMP_TUNE=1 ./expert_node --model "$MODEL" --port 7631 --name exec2-node >"$T/node.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -rf "$T"' EXIT
for _ in $(seq 1 600); do (exec 3<>/dev/tcp/127.0.0.1/7631) 2>/dev/null && break; sleep .1; done
HIDDEN=$(python3 -c "import json;print(json.load(open('$MODEL/config.json'))['hidden_size'])")
sleep 1
./test_exec2 127.0.0.1:7631 0 3 "$HIDDEN" || { cat "$T/node.log"; exit 1; }
./test_exec2 127.0.0.1:7631 1 5 "$HIDDEN" || { cat "$T/node.log"; exit 1; }
