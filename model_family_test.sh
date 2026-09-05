#!/usr/bin/env bash
# Colibri's adapter registry and Lumabri's family table must say the same
# thing, and the expected list has to be READ from Colibri rather than typed
# here — a test that compares two hand-written eights passes forever while
# Colibri quietly gains a ninth.
#
# Three claims:
#   1. every adapter Colibri exposes is registered by lmb_colibri_register_all
#      (Segment and Edge), and has a row in LMB_FAMILIES;
#   2. every row in LMB_FAMILIES names an adapter Colibri actually exposes;
#   3. every row has at least one exact model_type; no prefix debt is accepted.
set -euo pipefail
cd "$(dirname "$0")"
ENGINE="${ENGINE:-../colibri/c}"
fail() { echo "MODEL FAMILY TEST: FAIL — $*" >&2; exit 1; }

[[ -f "$ENGINE/segment_adapters.h" ]] || { echo "MODEL FAMILY TEST: SKIP (no $ENGINE)"; exit 0; }
[[ -f "$ENGINE/family_registry.py" ]] || fail "Colibri has no authoritative family_registry.py"

adapters_of() {  # $1 = header, $2 = segment|edge
    grep -oE "coli_[a-z0-9_]+_$2_adapter_register" "$1" |
        sed -E "s/^coli_(.*)_$2_adapter_register$/\1/" | sort -u
}
seg=$(adapters_of "$ENGINE/segment_adapters.h" segment)
edge=$(adapters_of "$ENGINE/edge_adapters.h" edge)
[[ -n "$seg" ]] || fail "no Segment adapters found in $ENGINE/segment_adapters.h"

# Colibri's own two lists must agree before we compare ourselves to them.
diff <(echo "$seg") <(echo "$edge") >/dev/null ||
    fail "Colibri exposes different Segment and Edge adapter sets:
$(diff <(echo "$seg") <(echo "$edge") || true)"

ours_seg=$(grep -oE "coli_[a-z0-9_]+_segment_adapter_register" segment_colibri.h |
           sed -E 's/^coli_(.*)_segment_adapter_register$/\1/' | sort -u)
ours_edge=$(grep -oE "coli_[a-z0-9_]+_edge_adapter_register" segment_colibri.h |
            sed -E 's/^coli_(.*)_edge_adapter_register$/\1/' | sort -u)

missing=$(comm -23 <(echo "$seg") <(echo "$ours_seg"))
[[ -z "$missing" ]] || fail "Colibri exposes Segment adapters Lumabri never registers: $(echo $missing).
Add them to lmb_colibri_register_all() and give each a row in lumabri_families.h."
extra=$(comm -13 <(echo "$seg") <(echo "$ours_seg"))
[[ -z "$extra" ]] || fail "Lumabri registers Segment adapters Colibri does not expose: $(echo $extra)"
missing=$(comm -23 <(echo "$edge") <(echo "$ours_edge"))
[[ -z "$missing" ]] || fail "Colibri exposes Edge adapters Lumabri never registers: $(echo $missing)"

# The family table and the debt list, parsed rather than eyeballed.
python3 - "$seg" "$ENGINE/family_registry.py" <<'PYEOF' || exit 1
import re, runpy, sys
src = open("lumabri_families.h", encoding="utf-8").read()
body = src[src.index("LMB_FAMILIES[] = {"):]
body = body[:body.index("\n};")]
rows = []
depth, cur = 0, ""
for ch in body[body.index("{") + 1:]:   # skip the array brace
    if ch == "{":
        depth += 1
        if depth == 1: cur = ""; continue
    if ch == "}":
        depth -= 1
        if depth == 0: rows.append(cur); continue
    if depth >= 1: cur += ch
table = {}
for row in rows:
    names = re.findall(r'"([^"]*)"', row)
    if not names: continue
    inner = re.findall(r"\{([^{}]*)\}", row)
    claims = [c for g in inner for c in re.findall(r'"([^"]+)"', g)]
    table[names[0]] = claims

expected = set(sys.argv[1].split())
registry = runpy.run_path(sys.argv[2])
official = {f.id: set(f.model_types) for f in registry["FAMILIES"]}
bad = []
if set(table) - expected:
    bad.append("LMB_FAMILIES names adapters Colibri does not expose: %s"
               % " ".join(sorted(set(table) - expected)))
if expected - set(table):
    bad.append("adapters with no row in LMB_FAMILIES: %s — a checkpoint of "
               "that family would be refused with no way to run it"
               % " ".join(sorted(expected - set(table))))
for name in sorted(expected & set(table)):
    if set(table[name]) != official.get(name, set()):
        bad.append("%s exact model_type aliases differ from Colibri:\n"
                   "  Lumabri: %s\n  Colibri: %s" %
                   (name, " ".join(sorted(table[name])) or "(none)",
                    " ".join(sorted(official.get(name, set()))) or "(none)"))
silent = {n for n, c in table.items() if not c}
if silent:
    bad.append("adapters without an exact model_type mapping: %s"
               % " ".join(sorted(silent)))
seen = {}
for name, claims in table.items():
    for c in claims:
        if c in seen:
            bad.append("model_type %r is claimed by both %s and %s"
                       % (c, seen[c], name))
        seen[c] = name
if bad:
    print("MODEL FAMILY TEST: FAIL — " + "\n".join(bad), file=sys.stderr)
    sys.exit(1)
print("  %d adapters, all exactly mapped" % len(table))
PYEOF

echo "MODEL FAMILY TEST: PASS ($(echo "$seg" | wc -w) adapters registered, mapped and unambiguous)"
