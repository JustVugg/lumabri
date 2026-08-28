#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
TRACKER_BIN=${TRACKER_BIN:-./tracker}
MAINTAINER_BIN=${MAINTAINER_BIN:-./maintainer}
DETAIL_BIN=${DETAIL_BIN:-./test_swarm_detail}
T=$(mktemp -d /tmp/lumabri-swarm-detail.XXXXXX)
PIDS=()
cleanup() {
    if ((${#PIDS[@]})); then kill "${PIDS[@]}" 2>/dev/null || true; fi
    if ((${#PIDS[@]})); then wait "${PIDS[@]}" 2>/dev/null || true; fi
    rm -rf "$T"
}
trap cleanup EXIT
wait_port() {
    for _ in $(seq 1 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && {
            exec 3<&-; exec 3>&-; return 0;
        }
        sleep .05
    done
    return 1
}
mkdir -p "$T/model"
printf '{"model_type":"olmoe"}\n' >"$T/model/config.json"
printf 'named-host-fixture\n' >"$T/model/weights.bin"
LUMABRI_PEER_BINDINGS="$T/bindings" "$TRACKER_BIN" --port 8090 >"$T/tracker.log" 2>&1 &
PIDS+=("$!"); wait_port 8090
LUMABRI_PEER_KEY="$T/peer.key" "$MAINTAINER_BIN" --root "$T/model" --port 8091 \
    --tracker 127.0.0.1:8090 --advertise 127.0.0.1:8091 \
    --name studio-gpu-storage --model-name tiny-live >"$T/peer.log" 2>&1 &
PIDS+=("$!"); wait_port 8091
for _ in $(seq 1 100); do
    grep -q '+ studio-gpu-storage' "$T/tracker.log" && break
    sleep .05
done
"$DETAIL_BIN" 127.0.0.1:8090 studio-gpu-storage tiny-live
echo 'SWARM DETAIL TEST: PASS (stable host name + storage counters)'
