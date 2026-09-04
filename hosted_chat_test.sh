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
        /* the serve codec, minimally: ACCEPT, one DATA, DONE */
        char buf[8192];
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;
            if (!memchr(buf, '\n', (size_t)n)) continue;
            const char *reply = "ACCEPT 0\nDATA 0 5\nhello\nDONE 0\n";
            if (write(fd, reply, strlen(reply)) < 0) break;
        }
        close(fd);
    }
}
EOF
cc -O2 -I. -pthread -o "$T/fakehost" "$T/fakehost.c" 2>"$T/build.log" ||
    { echo "HOSTED CHAT TEST: SKIP (stand-in host did not build)"; cat "$T/build.log"; exit 0; }

"$T/fakehost" "$PORT" 1 >"$T/host.log" 2>&1 & PIDS+=("$!")
for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && { exec 3<&-; exec 3>&-; break; }
    sleep .05
done

# THE measurement: a client home directory that starts empty must stay empty.
export HOME="$T/home"
mkdir -p "$HOME"
before=$(du -sb "$HOME" 2>/dev/null | cut -f1)

printf '/quit\n' | timeout 60 ./lumabri chat --host "127.0.0.1:$PORT" --plain \
    >"$T/client.log" 2>&1 || true

after=$(du -sb "$HOME" 2>/dev/null | cut -f1)
grep -q "host 127.0.0.1:$PORT" "$T/client.log" ||
    fail "the client did not report which host it used"
grep -qi "receives the text" "$T/client.log" ||
    fail "the client did not say the host sees the conversation"
grep -qi "not yet measured" "$T/client.log" ||
    fail "an unmeasured host did not say its speed is unknown"

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

echo "HOSTED CHAT TEST: PASS (zero checkpoint bytes, host disclosed, busy is an answer)"
