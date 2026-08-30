#!/usr/bin/env bash
# Phase 2 receives layer and expert IDs as unsigned wire values. Values above
# INT_MAX must be rejected before any cast or array lookup, by the executor,
# the chatter reading EMANIFEST_R, and a node reading EASSIGN_R.
set -euo pipefail
cd "$(dirname "$0")"

T=$(mktemp -d /tmp/lumabri-expert-input.XXXXXX)
PIDS=()
cleanup() {
    if [ "${#PIDS[@]}" -gt 0 ]; then
        kill "${PIDS[@]}" 2>/dev/null || true
        wait "${PIDS[@]}" 2>/dev/null || true
    fi
    rm -rf "$T"
}
trap cleanup EXIT

wait_port() {
    for _ in $(seq 1 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3<&-; return 0; }
        sleep 0.1
    done
    return 1
}

# A tiny engine contract keeps this test independent of colibri and models.
# Slot 0 routes and slot 1 is dense; its routed expert is the identity
# function. Only the protocol boundary matters.
cat > "$T/test_engine.h" <<'EOF'
#include <stdlib.h>
#include <string.h>
typedef struct { int unused; } LmbeSlot;
static const char *lmbe_engine_name(void) { return "test"; }
static void lmbe_open(const char *dir, int cap, int bits)
    { (void)dir; (void)cap; (void)bits; }
static int lmbe_effective_bits(int bits) { (void)bits; return 8; }
static int lmbe_n_slots(void) { return 2; }
static int lmbe_n_experts(void) { return 2; }
static int lmbe_hidden(void) { return 4; }
static int lmbe_inter(void) { return 4; }
static int lmbe_routed(int slot) { return slot == 0; }
static void lmbe_slot_init(LmbeSlot *s) { memset(s, 0, sizeof *s); }
static void lmbe_slot_load(int slot, int eid, LmbeSlot *s)
    { (void)slot; (void)eid; (void)s; }
static void *lmbe_scratch_new(int nrows) { (void)nrows; return malloc(1); }
static void lmbe_scratch_free(void *p) { free(p); }
static void lmbe_apply(const LmbeSlot *s, int slot, const float *x, float *out,
                       int nrows, const float *w, void *scratch) {
    (void)s; (void)slot; (void)w; (void)scratch;
    memcpy(out, x, (size_t)nrows * 4 * sizeof(float));
}
EOF

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
cc -O1 -g $SAN -fopenmp -pthread -I"$T" -I. \
    -DLMBE_ENGINE_HEADER='"test_engine.h"' expert_node.c \
    lumabri_machine.c \
    -o "$T/expert_node" -lm

# One helper can impersonate a peer or tracker and can inspect a real node.
cat > "$T/wire_test.c" <<'EOF'
#include "lumabri_proto.h"

/* An EMANIFEST_R the chatter will accept as far as the ownership table: the
 * v2 header must match the chatter's own engine, build profile and bits, or
 * the frame is refused for incompatibility before any index is looked at —
 * which would make this a test of the wrong rejection. wire_test and
 * client_test are built the same way, so both derive the same profile and
 * the default "unknown" engine / 8-bit experts. */
static void put_v2_header(LmbBuf *b) {
    char profile[LMB_BUILD_PROFILE_MAX];
    lmb_build_profile(profile, sizeof profile);
    lmb_buf_u32(b, LMB_EXPERT_MANIFEST_MAGIC);
    lmb_buf_str(b, LMBE_ENGINE_ID);
    lmb_buf_str(b, profile);
    lmb_buf_str(b, "test");            /* model name */
    lmb_buf_u32(b, 8);                 /* expert bits: LMBE_EXPECT_BITS default */
    lmb_buf_u32(b, 0);                 /* have_id = 0 */
}

static int bad_manifest(int port, int bad_eid) {
    int lfd = lmb_listen(port), fd; LmbMsg m = {0}; LmbBuf b = {0};
    if (lfd < 0) return 1;
    for (;;) {
        fd = accept(lfd, NULL, NULL);
        if (fd < 0) return 1;
        if (!lmb_recv(fd, &m) && m.op == LMB_EMANIFEST) break;
        lmb_msg_free(&m); close(fd);
    }
    put_v2_header(&b);
    lmb_buf_u32(&b, 2);
    lmb_buf_u32(&b, 0); lmb_buf_u32(&b, 0);
    lmb_buf_u32(&b, bad_eid ? 0 : UINT32_MAX);
    lmb_buf_u32(&b, bad_eid ? UINT32_MAX : 0);
    lmb_buf_u32(&b, 4);                /* peer hidden */
    int rc = lmb_send(fd, LMB_EMANIFEST_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p); lmb_msg_free(&m); close(fd); close(lfd); return rc != 0;
}

static int bad_assignment(int port) {
    int lfd = lmb_listen(port), fd; LmbMsg m = {0}; LmbBuf b = {0};
    if (lfd < 0) return 1;
    for (;;) {
        fd = accept(lfd, NULL, NULL);
        if (fd < 0) return 1;
        if (!lmb_recv(fd, &m) && m.op == LMB_EASSIGN) break;
        lmb_msg_free(&m); close(fd);
    }
    lmb_buf_u32(&b, 2);
    lmb_buf_u32(&b, 0); lmb_buf_u32(&b, 0);
    lmb_buf_u32(&b, UINT32_MAX); lmb_buf_u32(&b, UINT32_MAX);
    int rc = lmb_send(fd, LMB_EASSIGN_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p); lmb_msg_free(&m); close(fd); close(lfd); return rc != 0;
}

static int dense_assignment(int port) {
    int lfd = lmb_listen(port), fd; LmbMsg m = {0}; LmbBuf b = {0};
    if (lfd < 0) return 1;
    for (;;) {
        fd = accept(lfd, NULL, NULL);
        if (fd < 0) return 1;
        if (!lmb_recv(fd, &m) && m.op == LMB_EASSIGN) break;
        lmb_msg_free(&m); close(fd);
    }
    lmb_buf_u32(&b, 1); lmb_buf_u32(&b, 1); lmb_buf_u32(&b, 0);
    int rc = lmb_send(fd, LMB_EASSIGN_R, b.p, (uint32_t)b.len, NULL, 0);
    free(b.p); lmb_msg_free(&m); close(fd); close(lfd); return rc != 0;
}

/* Skip the v2 EMANIFEST_R header and read the expert count that follows. */
static int manifest_count(const char *addr, uint32_t expected) {
    LmbMsg m = {0};
    if (lmb_request(addr, LMB_EMANIFEST, NULL, 0, &m) ||
        m.op != LMB_EMANIFEST_R) return 1;
    LmbCur c = { m.body, m.body_len, 0 };
    uint32_t magic = 0, bits = 0, have_id = 0, has_sig = 0, n = 0;
    char engine[64], profile[LMB_BUILD_PROFILE_MAX], model[64];
    uint8_t root[LMB_MODEL_ROOT_LEN], sig[64];
    int rc = lmb_cur_u32(&c, &magic) || magic != LMB_EXPERT_MANIFEST_MAGIC ||
             lmb_cur_str(&c, engine, sizeof engine) ||
             lmb_cur_str(&c, profile, sizeof profile) ||
             lmb_cur_str(&c, model, sizeof model) ||
             lmb_cur_u32(&c, &bits) || lmb_cur_u32(&c, &have_id);
    if (!rc && have_id)
        rc = lmb_cur_bytes(&c, root, sizeof root) || lmb_cur_u32(&c, &has_sig) ||
             (has_sig && lmb_cur_bytes(&c, sig, sizeof sig));
    if (!rc) rc = lmb_cur_u32(&c, &n) || n != expected;
    lmb_msg_free(&m); return rc;
}

static int one_exec(const char *addr, uint32_t layer, uint32_t eid,
                    uint32_t expect_op) {
    int fd = lmb_connect(addr); LmbBuf b = {0}; LmbMsg m = {0};
    float x[4] = {1, 2, 3, 4};
    if (fd < 0) return 1;
    lmb_buf_u32(&b, layer); lmb_buf_u32(&b, eid);
    lmb_buf_u32(&b, 4); lmb_buf_u32(&b, 1);
    int rc = lmb_send(fd, LMB_EXEC, b.p, (uint32_t)b.len, x, sizeof x) ||
             lmb_recv(fd, &m) || m.op != expect_op;
    if (!rc && expect_op == LMB_EXEC_R)
        rc = m.pay_len != sizeof x || memcmp(m.pay, x, sizeof x);
    free(b.p); lmb_msg_free(&m); close(fd); return rc;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "bad-manifest"))
        return bad_manifest(atoi(argv[2]), 0);
    if (argc == 3 && !strcmp(argv[1], "bad-manifest-eid"))
        return bad_manifest(atoi(argv[2]), 1);
    if (argc == 3 && !strcmp(argv[1], "bad-assignment"))
        return bad_assignment(atoi(argv[2]));
    if (argc == 3 && !strcmp(argv[1], "dense-assignment"))
        return dense_assignment(atoi(argv[2]));
    if (argc == 4 && !strcmp(argv[1], "manifest-count"))
        return manifest_count(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
    if (argc == 3 && !strcmp(argv[1], "exec")) {
        if (one_exec(argv[2], UINT32_MAX, 0, LMB_ERR)) return 1;
        if (one_exec(argv[2], 0, UINT32_MAX, LMB_ERR)) return 1;
        return one_exec(argv[2], 0, 0, LMB_EXEC_R);
    }
    return 2;
}
EOF
cc -O2 -w -I. "$T/wire_test.c" -o "$T/wire_test" -lpthread

# The chatter side is header-only. A configured bad peer must be rejected
# with its protocol diagnostic, rather than reaching L.own with a huge index.
cat > "$T/client_test.c" <<'EOF'
#include "lumabri_client.h"
int main(int argc, char **argv) {
    setenv("LUMABRI_EXPERTS", argv[1], 1);
    lumi_init(2, 2, 4);
    return 0;
}
EOF
cc -O1 -g $SAN -I. "$T/client_test.c" -o "$T/client_test" -lpthread

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export ASAN_OPTIONS
export UBSAN_OPTIONS

echo "· 1) EXEC rejects an unsigned index before looking up the expert"
"$T/expert_node" --model "$T/model" --port 7581 --name exec-test \
    > "$T/exec.log" 2>&1 & PIDS+=($!)
wait_port 7581
"$T/wire_test" exec 127.0.0.1:7581
kill -0 "${PIDS[-1]}"
! grep -q "Sanitizer" "$T/exec.log"
! grep -q "runtime error:" "$T/exec.log"
echo "   ✓ rejected, then served a valid request on a fresh connection"

echo "· 2) EMANIFEST_R rejects an unsigned layer from a peer"
"$T/wire_test" bad-manifest 7582 > "$T/peer.log" 2>&1 & PIDS+=($!)
wait_port 7582
set +e
"$T/client_test" 127.0.0.1:7582 > "$T/client.out" 2> "$T/client.err"
RC=$?
set -e
[ "$RC" -ne 0 ] && grep -q "incompatible manifest" "$T/client.err" && \
    ! grep -q "Sanitizer" "$T/client.err" && \
    ! grep -q "runtime error:" "$T/client.err" || {
    cat "$T/client.err"; echo "   chatter did not reject the bad manifest cleanly"; exit 1; }
echo "   ✓ peer skipped without changing the ownership table"

echo "· 2b) EMANIFEST_R also rejects an unsigned expert ID"
"$T/wire_test" bad-manifest-eid 7587 > "$T/peer-eid.log" 2>&1 & PIDS+=($!)
wait_port 7587
set +e
"$T/client_test" 127.0.0.1:7587 > "$T/client-eid.out" 2> "$T/client-eid.err"
RC=$?
set -e
[ "$RC" -ne 0 ] && grep -q "incompatible manifest" "$T/client-eid.err" && \
    ! grep -q "Sanitizer" "$T/client-eid.err" && \
    ! grep -q "runtime error:" "$T/client-eid.err" || {
    cat "$T/client-eid.err"; echo "   chatter did not reject the bad expert ID"; exit 1; }
echo "   ✓ expert ID rejected before the ownership lookup"

echo "· 3) EASSIGN_R is validated completely before replacing the local set"
"$T/wire_test" bad-assignment 7583 > "$T/tracker.log" 2>&1 & PIDS+=($!)
wait_port 7583
"$T/expert_node" --model "$T/model" --port 7584 --name assign-test \
    --tracker 127.0.0.1:7583 --hold 2 > "$T/assign.log" 2>&1 & PIDS+=($!)
wait_port 7584
"$T/wire_test" manifest-count 127.0.0.1:7584 2
grep -q "invalid expert assignment" "$T/assign.log"
! grep -q "Sanitizer" "$T/assign.log"
! grep -q "runtime error:" "$T/assign.log"
echo "   ✓ malformed reply refused; the original routed experts remain held"

echo "· 4) EASSIGN_R cannot assign an expert to a dense layer"
"$T/wire_test" dense-assignment 7585 > "$T/dense-tracker.log" 2>&1 & PIDS+=($!)
wait_port 7585
"$T/expert_node" --model "$T/model" --port 7586 --name dense-test \
    --tracker 127.0.0.1:7585 --hold 1 > "$T/dense.log" 2>&1 & PIDS+=($!)
wait_port 7586
"$T/wire_test" manifest-count 127.0.0.1:7586 2
grep -q "invalid expert assignment" "$T/dense.log"
! grep -q "Sanitizer" "$T/dense.log"
! grep -q "runtime error:" "$T/dense.log"
echo "   ✓ dense-layer reply refused before reaching the engine loader"

echo "LUMABRI EXPERT INPUT TEST: PASS"
