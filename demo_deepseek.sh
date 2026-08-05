#!/usr/bin/env bash
# lumabri × deepseek — the first real demo.
#
# The DeepSeek-V4 checkpoint lives ONLY on two "peers" (maintainers serving
# disjoint halves of the shards); the engine runs against a virtual directory
# that does not exist on disk. Every byte it reads is fetched from a peer on
# first touch, mirrored to the local cache, and read locally forever after.
# The engine binary is completely unmodified.
#
#   ./demo_deepseek.sh "your prompt"
#
# Env overrides: MODEL, ENGINE, CACHE, NGEN, BLOCK_MIB.
set -euo pipefail
cd "$(dirname "$0")"

MODEL="${MODEL:-$HOME/deepseek_v4}"
ENGINE="${ENGINE:-/mnt/c/Users/User/Desktop/moe-stream/c/deepseek}"
VROOT="${VROOT:-$HOME/lumabri_vroot/deepseek_v4}"     # never exists on disk
CACHE="${CACHE:-$HOME/lumabri_cache/deepseek_v4}"
PROMPT="${1:-Ciao! Presentati in una frase.}"
NGEN="${NGEN:-48}"

[ -d "$MODEL" ] || { echo "model dir not found: $MODEL"; exit 1; }
[ -x "$ENGINE" ] || { echo "engine not found: $ENGINE"; exit 1; }

make -s all

PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; }
trap cleanup EXIT

./tracker --port 7300 & PIDS+=($!)
sleep 0.3
# peer A: even shards + every json (config, tokenizer, index)
./maintainer --root "$MODEL" --port 7301 --tracker 127.0.0.1:7300 --name peer-even \
             --include '*[02468]-of-*.safetensors' --include '*.json' & PIDS+=($!)
# peer B: odd shards
./maintainer --root "$MODEL" --port 7302 --tracker 127.0.0.1:7300 --name peer-odd \
             --include '*[13579]-of-*.safetensors' & PIDS+=($!)
sleep 0.7

echo
echo "· the model directory the engine will open is $VROOT (it does not exist)"
echo "· bytes live on peer-even (:7301) and peer-odd (:7302); mirror in $CACHE"
echo

LD_PRELOAD="$PWD/liblumabri.so" \
LUMABRI_VROOT="$VROOT" \
LUMABRI_CACHE="$CACHE" \
LUMABRI_TRACKER=127.0.0.1:7300 \
LUMABRI_BLOCK_MIB="${BLOCK_MIB:-8}" \
LUMABRI_STATS=5 \
"$ENGINE" "$VROOT" "$PROMPT" --max-tokens "$NGEN"

echo
echo "· local mirror after this run:"
du -sh "$CACHE" 2>/dev/null || true
