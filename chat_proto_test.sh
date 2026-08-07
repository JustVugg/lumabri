#!/usr/bin/env bash
# lumabri — the chat front end against BOTH engine dialects, and against an
# engine that dies.
#
#   1) FRAMED   colibri/glm/deepseek/kimi/inkling speak SERVE mode:
#               \x01\x01READY\x01\x01 once, tokens streamed, \x01\x01END\x01\x01
#               plus a STAT line per turn, \x02RESET to forget. The front end
#               used to wait for olmoe's "> " prompt instead, which never
#               comes — the whole reason a GLM swarm reported "engine did not
#               start" after loading perfectly well.
#   2) LINE     olmoe's dialect still works: the two must coexist.
#   3) DEATH    an engine that exits must say WHY: exit code and its own last
#               words, not a shrug.
set -euo pipefail
cd "$(dirname "$0")"

make -s lumabri
T=$(mktemp -d /tmp/lumabri-proto.XXXXXX)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/model"
printf '{"model_type":"fake"}' > "$T/model/config.json"

# --- a fake engine that speaks the framed SERVE protocol ------------------
cat > "$T/framed" <<'EOF'
#!/usr/bin/env bash
[ -n "${SERVE:-}" ] || { echo "SERVE not set: the front end did not ask for serve mode" >&2; exit 3; }
[ -n "${SNAP:-}" ]  || { echo "SNAP not set" >&2; exit 3; }
echo "[FAKE] loading from $SNAP, cap=$1, ngen=${NGEN:-?}" >&2
printf '\x01\x01READY\x01\x01\nSTAT 0 0.00 0.0 1.00\n'
while IFS= read -r line; do
    if [ "$line" = $'\x02RESET' ]; then
        printf '\x01\x01END\x01\x01\nSTAT 0 0.00 0.0 1.00\n'
        continue
    fi
    # stream it back a word at a time, the way a real engine emits tokens
    for w in $line; do printf '%s ' "$w"; done
    printf '\n\x01\x01END\x01\x01\nSTAT 7 4.25 91.5 2.75\n'
done
EOF
chmod +x "$T/framed"

echo "· 1) framed dialect: READY, streamed tokens, END + STAT"
OUT=$(printf 'ciao mondo\n/reset\nancora\n/quit\n' | \
      ./lumabri chat --local "$T/model" --engine "$T/framed" --plain --max-new 16 2>&1)
echo "$OUT" | grep -q "ciao mondo" || { echo "   the reply never reached the user"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "4.2 tok/s"  || { echo "   STAT was not read back"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "2.8 GB residenti" || { echo "   STAT rss missing"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "nuova conversazione" || { echo "   /reset did not round-trip"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "ancora" || { echo "   the turn after /reset was lost"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "disco locale" || { echo "   --local not honoured"; echo "$OUT"; exit 1; }
echo "   ✓ two turns and a reset, streamed, with the engine's own numbers"

echo "· 2) line dialect (olmoe) still works"
cat > "$T/liner" <<'EOF'
#!/usr/bin/env bash
[ -n "${CHAT:-}" ] || { echo "CHAT not set" >&2; exit 3; }
printf 'ready\n> '
while IFS= read -r line; do printf 'echo: %s\n> ' "$line"; done
EOF
chmod +x "$T/liner"
OUT=$(printf 'salve\n/quit\n' | \
      ./lumabri chat --local "$T/model" --engine "$T/liner" --plain 2>&1)
echo "$OUT" | grep -q "echo: salve" || { echo "   line protocol broke"; echo "$OUT"; exit 1; }
echo "   ✓ olmoe's prompt dialect untouched"

echo "· 3) an engine that dies must say why"
cat > "$T/dead" <<'EOF'
#!/usr/bin/env bash
echo "config.json: unsupported quantization iq3_xxs" >&2
echo "aborting" >&2
exit 42
EOF
chmod +x "$T/dead"
set +e
OUT=$(printf '/quit\n' | ./lumabri chat --local "$T/model" --engine "$T/dead" --plain 2>&1)
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "   a dead engine reported success"; exit 1; }
echo "$OUT" | grep -q "codice 42" || { echo "   the exit code was swallowed"; echo "$OUT"; exit 1; }
echo "$OUT" | grep -q "unsupported quantization" || {
    echo "   the engine's own error was swallowed — the bug this test exists for"
    echo "$OUT"; exit 1; }
echo "   ✓ exit code and the engine's last words both survive"

echo "LUMABRI CHAT PROTOCOL TEST: PASS"
