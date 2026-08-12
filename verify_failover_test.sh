#!/usr/bin/env bash
# A spot-check must cover every answer the chatter is willing to use, not just
# the one the fast path produced. The failover path returns an answer too, and
# on a NAT'd swarm the relay behind it can be the only way an expert is
# reachable at all — an answer nobody can check is exactly what this client
# exists not to emit.
set -euo pipefail
cd "$(dirname "$0")"

make -s test_verify_failover

echo "· a lying replica reached through failover is caught"
set +e
OUT=$(./test_verify_failover liar 2>&1)
RC=$?
set -e
if [ "$RC" -eq 0 ]; then
    echo "   the corrupted failover answer was accepted"
    echo "$OUT"
    exit 1
fi
grep -q "INTEGRITY FAILURE" <<<"$OUT" || {
    echo "   the run stopped, but not with the integrity error"
    echo "$OUT"
    exit 1
}
echo "   ✓ INTEGRITY FAILURE, and the bytes never reached the model"

echo "· an honest failover is checked and not mistaken for an attack"
./test_verify_failover honest

echo "LUMABRI VERIFY FAILOVER TEST: PASS"
