#!/usr/bin/env bash
# The promise of a hosted chat is a number: zero bytes of the checkpoint on
# the machine you type at. Anything less is a smaller download, not a
# different product, so this test counts the bytes.
#
# It also checks the parts that only exist because the engine is now a
# socket: a second client is refused rather than silently queued, and /quit
# closes the session instead of leaking it.
set -euo pipefail
cd "$(dirname "$0")"
[[ -x ./lumabri ]] || { echo "HOSTED CHAT TEST: SKIP (build lumabri)"; exit 0; }

PORT="${PORT:-8440}"
T=$(mktemp -d /tmp/lumabri-hosted.XXXXXX)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true
            wait "${PIDS[@]}" 2>/dev/null || true; rm -rf "$T"; }
fail() { echo "HOSTED CHAT TEST: FAIL — $*" >&2
         for l in "$T"/*.log; do [[ -f $l ]] || continue
             echo "--- $(basename "$l")" >&2; tail -25 "$l" >&2; done; exit 1; }
trap cleanup EXIT

# A stand-in host: it speaks the greeting and the serve codec, so this test
# exercises OUR client and OUR lifecycle without needing a checkpoint. The
# real engine is covered by the phase-2 and segment tests.
cat > "$T/fakehost.c" <<'EOF'
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "lumabri_proto.h"
#include "lumabri_sign.h"
#include "lumabri_secure.h"
int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8441;
    int busy_after = argc > 2 ? atoi(argv[2]) : 1;
    if (lmb_secure_init()) return 1;
    int lfd = lmb_listen(port);
    if (lfd < 0) return 1;
    fprintf(stderr, "fakehost listening\n");
    int served = 0;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) return 1;
        LmbMsg m = {0};
        if (lmb_secure_server(fd) || lmb_recv(fd, &m) || m.op != LMB_HOST_HELLO) {
            lmb_msg_free(&m); close(fd); continue;
        }
        lmb_msg_free(&m);
        LmbBuf b = {0};
        lmb_buf_str(&b, "olmoe");
        lmb_buf_str(&b, "olmoe");
        lmb_buf_str(&b, "cpu");
        lmb_buf_u32(&b, served >= busy_after ? 0u : 1u);
        lmb_buf_u32(&b, 0u);                   /* never measured */
        lmb_send(fd, LMB_HOST_HELLO_R, b.p, (uint32_t)b.len, NULL, 0);
        free(b.p);
        if (served >= busy_after) { close(fd); continue; }
        served++;
        /* the serve codec remains inside HOST_STREAM frames. */
        for (;;) {
            LmbMsg stream = {0};
            if (lmb_recv(fd, &stream) || stream.op != LMB_HOST_STREAM ||
                !stream.pay_len) { lmb_msg_free(&stream); break; }
            lmb_msg_free(&stream);
            const char *reply = "ACCEPT 0\nDATA 0 5\nhello\nDONE 0\n";
            if (lmb_send(fd, LMB_HOST_STREAM, NULL, 0,
                         reply, (uint32_t)strlen(reply))) break;
        }
        lmb_close(fd);
    }
}
EOF
cc -O2 -I. -pthread -o "$T/fakehost" "$T/fakehost.c" 2>"$T/build.log" ||
    { echo "HOSTED CHAT TEST: SKIP (stand-in host did not build)"; cat "$T/build.log"; exit 0; }

HOME="$T/fake-host-home" LUMABRI_ENCRYPT=1 \
LUMABRI_PEER_KEY="$T/fake-host.key" \
LUMABRI_KNOWN_HOSTS="$T/fake-host.hosts" \
    "$T/fakehost" "$PORT" 1 >"$T/host.log" 2>&1 & PIDS+=("$!")
for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && { exec 3<&-; exec 3>&-; break; }
    sleep .05
done

# THE measurement: a client home directory that starts empty must stay empty.
export HOME="$T/home"
mkdir -p "$HOME"
export LUMABRI_ENCRYPT=1
export LUMABRI_PEER_KEY="$T/client.key"
export LUMABRI_KNOWN_HOSTS="$T/client.hosts"
before=$(du -sb "$HOME" 2>/dev/null | cut -f1)

printf 'hi\n/quit\n' | timeout 60 ./lumabri chat --host "127.0.0.1:$PORT" --plain \
    >"$T/client.log" 2>&1 || true

after=$(du -sb "$HOME" 2>/dev/null | cut -f1)
grep -q "host 127.0.0.1:$PORT" "$T/client.log" ||
    fail "the client did not report which host it used"
grep -qi "receives the text" "$T/client.log" ||
    fail "the client did not say the host sees the conversation"
grep -qi "not yet measured" "$T/client.log" ||
    fail "an unmeasured host did not say its speed is unknown"
grep -q "hello" "$T/client.log" ||
    fail "the encrypted HOST_STREAM did not carry a generated reply"

# Not one byte of model, mirror or CAS.
(( after - before < 65536 )) ||
    fail "a hosted chat wrote $(( after - before )) bytes into HOME; the whole
    claim is that it writes no checkpoint at all"
[[ ! -d "$HOME/.lumabri/models" && ! -d "$HOME/.lumabri/cas" ]] ||
    fail "a hosted chat created a mirror or CAS directory"

# A second client is refused with an answer, not left waiting.
printf '/quit\n' | timeout 30 ./lumabri chat --host "127.0.0.1:$PORT" --plain \
    >"$T/second.log" 2>&1 || true
grep -qE "0 sessions free" "$T/second.log" ||
    fail "a busy host did not tell the second client it was full"

# The real host loop, with a tiny stand-in engine. This exercises admission in
# Lumabri itself; the fake host above exercises the thin client in isolation.
# It has a distinct identity, so use a distinct TOFU database too; accepting a
# second key for the same address in the first database would weaken the test.
export LUMABRI_KNOWN_HOSTS="$T/real-client.hosts"
mkdir -p "$T/model"
printf '%s\n' '{"model_type":"qwen3_5_moe","text_config":{"model_type":"qwen3_5_moe"}}' >"$T/model/config.json"
cat >"$T/qwen36.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fputs("\1\1READY\1\1\nSTAT 0 0 0 0\n", stdout);
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char id[64]; unsigned slot, max_new; size_t bytes; float t, p;
        if (sscanf(line, "SUBMIT %63s %u %zu %u %f %f",
                   id, &slot, &bytes, &max_new, &t, &p) != 6) continue;
        char *payload = malloc(bytes + 2);
        if (!payload || fread(payload, 1, bytes + 1, stdin) != bytes + 1)
            return 1;
        payload[bytes] = 0;
        if (!strstr(payload, "<|im_start|>assistant\n<think>\n")) return 2;
        free(payload);
        printf("ACCEPT %s\nDATA %s 5\nhello\nDONE %s\n", id, id, id);
    }
    return 0;
}
EOF
cc -O2 -o "$T/qwen36" "$T/qwen36.c"
REAL_PORT=$(( PORT + 1 ))
mkdir -p "$T/real-home"
HOME="$T/real-home" LUMABRI_PEER_KEY="$T/real-host.key" \
LUMABRI_KNOWN_HOSTS="$T/real-host.hosts" \
    ./lumabri host --local "$T/model" --engine "$T/qwen36" \
    --port "$REAL_PORT" --max-new 8 >"$T/real-host.log" 2>&1 & PIDS+=("$!")
for _ in $(seq 1 200); do
    grep -q "host ready" "$T/real-host.log" 2>/dev/null && break
    sleep .05
done
grep -q "host ready" "$T/real-host.log" || fail "the real host did not start"

( sleep 3; printf '/quit\n' ) | timeout 20 ./lumabri chat \
    --host "127.0.0.1:$REAL_PORT" --plain >"$T/held.log" 2>&1 & held=$!
sleep .5
started=$(date +%s)
printf '/quit\n' | timeout 10 ./lumabri chat --host "127.0.0.1:$REAL_PORT" \
    --plain >"$T/real-busy.log" 2>&1 || true
elapsed=$(( $(date +%s) - started ))
grep -qi "busy (0 sessions free)" "$T/real-busy.log" ||
    fail "the real host did not refuse its second client with BUSY"
(( elapsed < 3 )) || fail "BUSY was queued for $elapsed seconds instead of immediate"
wait "$held" || true

echo "HOSTED CHAT TEST: PASS (zero checkpoint bytes, authenticated encrypted stream, real BUSY)"
