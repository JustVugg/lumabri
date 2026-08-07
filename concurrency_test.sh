#!/usr/bin/env bash
# lumabri — does one server hold up when several people chat at once?
#
# The question is not "does it answer" but "does anyone get starved". So the
# same generation is run by N chatters simultaneously and each one's wall
# time is recorded; what matters is the SPREAD between the fastest and the
# slowest, and how the total degrades as N grows.
#
# Where contention actually lives, so the numbers can be read honestly:
#
#   bytes     the maintainer answers reads with positional pread on shared
#             fds — no lock on the read path, and the page cache serves
#             every client the same hot bytes. This scales.
#   experts   the expert node runs one thread per connection, but a cache
#             MISS holds a single global loader lock, because the engine
#             loaders are engine-internal state and not re-entrant. Cold
#             experts therefore serialise across clients; hot ones do not.
#             --cache is the knob: size it so the working set fits.
#
# On one machine the clients also compete for the same cores as the server,
# so absolute numbers here are pessimistic. The spread is the real signal.
set -euo pipefail
cd "$(dirname "$0")"

MODEL="${MODEL:-$PWD/tiny_olmoe}"
PORT="${PORT:-7360}"
CLIENTS="${CLIENTS:-1 2 4}"
GEN="${GEN:-8}"
ENGINES="${ENGINES:-../moe-stream/c}"

[ -f "$MODEL/config.json" ] || make -s fixture
make -s all

T=$(mktemp -d /tmp/lumabri-conc.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

./lumabri serve --model "$MODEL" --port "$PORT" --exec-cache 128 \
    > "$T/serve.log" 2>&1 & PIDS+=($!)
for _ in $(seq 1 400); do
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && { exec 3<&-; break; }
    sleep 0.2
done
sleep 3

# one warm-up so every timed client starts from the same warm mirror — the
# first chatter always pays for the dense weights and would dominate
printf 'ciao\n/quit\n' | ./lumabri chat --tracker "127.0.0.1:$PORT" \
    --engines-dir "$ENGINES" --plain --max-new "$GEN" --role chat \
    > "$T/warm.log" 2>&1 || true

echo
printf "%8s  %10s  %10s  %10s  %10s\n" clients fastest slowest spread total
for n in $CLIENTS; do
    for i in $(seq 1 "$n"); do
        ( start=$(date +%s.%N)
          printf 'ciao\n/quit\n' | ./lumabri chat --tracker "127.0.0.1:$PORT" \
              --engines-dir "$ENGINES" --plain --max-new "$GEN" --role chat \
              > "$T/c$i.log" 2>&1
          end=$(date +%s.%N)
          echo "$end - $start" | bc > "$T/t$i" ) &
    done
    wait
    read -r fast slow tot < <(cat "$T"/t* | sort -n | awk '
        {v[NR]=$1; s+=$1}
        END {printf "%.1f %.1f %.1f", v[1], v[NR], s/NR}')
    spread=$(echo "$slow - $fast" | bc)
    printf "%8s  %9ss  %9ss  %9ss  %9ss\n" "$n" "$fast" "$slow" "$spread" "$tot"
    rm -f "$T"/t*
done

echo
echo "spread = quanto il piu' lento paga rispetto al piu' veloce."
echo "Se cresce con i client, qualcuno sta aspettando; se resta piatto, no."
