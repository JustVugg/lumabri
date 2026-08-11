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
# the executor is not part of `all` (it needs an engine checkout), and a stale
# one silently benchmarks yesterday's code
make -s expert_node ENGINE="$ENGINES"

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
# The tracker listens first.  The byte peer opens PORT+1 only after hashing
# the model, and the executor opens PORT+2 after loading it.  Waiting for all
# three prevents the warm-up from racing a cold `lumabri serve` startup.
for p in $((PORT + 1)) $((PORT + 2)); do
    ready=0
    for _ in $(seq 1 400); do
        (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && { exec 3<&-; ready=1; break; }
        sleep 0.2
    done
    [ "$ready" -eq 1 ] || { echo "server component on port $p never became ready"; exit 1; }
done

# Two processes must be allowed to bind a brand-new shared mirror to the same
# checkpoint at once.  This used to make the loser wait for the winner's whole
# process lifetime after both tried to upgrade the cache lock.
echo "· simultaneous cold mirror initialization"
IPIDS=()
for i in 1 2; do
    env LD_PRELOAD="$PWD/liblumabri.so" LUMABRI_CACHE="$T/cache" \
        LUMABRI_VROOT="$T/vroot" LUMABRI_TRACKER="127.0.0.1:$PORT" \
        LUMABRI_MODEL=tiny_olmoe LUMABRI_BLOCK_MIB=1 \
        head -c 1 "$T/vroot/config.json" > "$T/init$i" 2> "$T/init$i.err" &
    IPIDS+=($!)
done
for pid in "${IPIDS[@]}"; do
    wait "$pid" || { cat "$T"/init*.err; exit 1; }
done
cmp "$T/init1" "$T/init2" || { echo "cold initializers read different bytes"; exit 1; }
echo "  ✓ both initializers completed on one checkpoint"

# one warm-up so every timed client starts from the same warm mirror — the
# first chatter always pays for the dense weights and would dominate
printf 'ciao\n/quit\n' | env LUMABRI_CACHE="$T/cache" LUMABRI_VROOT="$T/vroot" \
    ./lumabri chat --tracker "127.0.0.1:$PORT" \
    --engines-dir "$ENGINES" --plain --max-new "$GEN" --role chat \
    > "$T/warm.log" 2>&1 || { cat "$T/warm.log"; exit 1; }

echo
printf "%8s  %10s  %10s  %10s  %10s\n" clients fastest slowest spread mean
for n in $CLIENTS; do
    # wait on THESE clients only: a bare `wait` also waits for the server
    # started above, which never exits — the first version of this script
    # hung there and printed nothing but the header
    CPIDS=()
    for i in $(seq 1 "$n"); do
        ( start=$(date +%s.%N)
          printf 'ciao\n/quit\n' | env LUMABRI_CACHE="$T/cache" LUMABRI_VROOT="$T/vroot" \
              ./lumabri chat --tracker "127.0.0.1:$PORT" \
              --engines-dir "$ENGINES" --plain --max-new "$GEN" --role chat \
              > "$T/c$i.log" 2>&1
          end=$(date +%s.%N)
          echo "$end - $start" | bc > "$T/t$i" ) &
        CPIDS+=($!)
    done
    FAILED=0
    for pid in "${CPIDS[@]}"; do wait "$pid" || FAILED=1; done
    if [ "$FAILED" -ne 0 ]; then
        echo "one or more chatter processes failed"
        for log in "$T"/c*.log; do echo "== $log =="; tail -40 "$log"; done
        exit 1
    fi
    STATS=$(cat "$T"/t* 2>/dev/null | sort -n | awk '
        {v[NR]=$1; s+=$1}
        END {if (NR) printf "%.1f %.1f %.1f %.1f", v[1], v[NR], v[NR]-v[1], s/NR;
             else printf "0 0 0 0"}')
    set -- $STATS
    printf "%8s  %9ss  %9ss  %9ss  %9ss\n" "$n" "$1" "$2" "$3" "$4"
    rm -f "$T"/t*
done

echo
echo "spread = quanto il piu' lento paga rispetto al piu' veloce."
echo "Se cresce con i client, qualcuno sta aspettando; se resta piatto, no."
