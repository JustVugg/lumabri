#!/usr/bin/env bash
# Operator keys are irreplaceable local authority. Generation must never
# rotate an existing key or follow an output symlink implicitly.
set -euo pipefail
cd "$(dirname "$0")"

make -s lumabri
T=$(mktemp -d /tmp/lumabri-key.XXXXXX)
cleanup() { rm -rf "$T"; }
trap cleanup EXIT

echo "· a fresh operator keypair has the promised shape and permissions"
umask 000
./lumabri key --out "$T/fresh" >"$T/fresh.out"
MODE=$(stat -c %a "$T/fresh.key" 2>/dev/null || stat -f %Lp "$T/fresh.key")
[ "$MODE" = 600 ] || {
    echo "   secret key is not mode 0600"; exit 1; }
[ "$(wc -c < "$T/fresh.key")" -eq 129 ] || {
    echo "   secret key is not 128 hex bytes plus newline"; exit 1; }
[ "$(wc -c < "$T/fresh.pub")" -eq 65 ] || {
    echo "   public key is not 64 hex bytes plus newline"; exit 1; }
grep -Eq '^[0-9a-f]{128}$' "$T/fresh.key" || {
    echo "   secret key is not hexadecimal"; exit 1; }
grep -Eq '^[0-9a-f]{64}$' "$T/fresh.pub" || {
    echo "   public key is not hexadecimal"; exit 1; }
[ "$(tail -c 65 "$T/fresh.key")" = "$(cat "$T/fresh.pub")" ] || {
    echo "   public file is not the public half of the secret key"; exit 1; }
grep -Fq "$(cat "$T/fresh.pub")" "$T/fresh.out" || {
    echo "   command did not print the generated public key"; exit 1; }
grep -Fqx "    LUMABRI_PUBKEY=$(cat "$T/fresh.pub") lumabri chat --tracker HOST:7300" \
    "$T/fresh.out" || {
    echo "   generated verifier command contains the wrong public key"; exit 1; }
if grep -Fq "$(cat "$T/fresh.key")" "$T/fresh.out"; then
    echo "   command printed the secret key"; exit 1
fi
if grep -Fq "$(cut -c1-64 "$T/fresh.key")" "$T/fresh.out"; then
    echo "   command printed the private seed"; exit 1
fi
echo "   ✓ complete pair created; secret mode is exactly 0600"

echo "· an existing keypair is never rotated implicitly"
cp "$T/fresh.key" "$T/original.key"
cp "$T/fresh.pub" "$T/original.pub"
if ./lumabri key --out "$T/fresh" >"$T/existing.out" 2>"$T/existing.err"; then
    echo "   existing operator keys were overwritten"; exit 1
fi
cmp "$T/original.key" "$T/fresh.key" && cmp "$T/original.pub" "$T/fresh.pub" || {
    echo "   failed generation changed an existing keypair"; exit 1; }
echo "   ✓ rerunning the command refuses and preserves both files"

echo "· an existing public destination is preserved without an orphan secret"
printf 'public destination canary\n' > "$T/public-only.pub"
if ./lumabri key --out "$T/public-only" >"$T/public-only.out" 2>"$T/public-only.err"; then
    echo "   existing public destination was overwritten"; exit 1
fi
[ "$(cat "$T/public-only.pub")" = "public destination canary" ] || {
    echo "   existing public destination was modified"; exit 1; }
[ ! -e "$T/public-only.key" ] || {
    echo "   orphan secret remained after public destination refusal"; exit 1; }
echo "   ✓ public destination stayed intact and temporary secret was removed"

echo "· key output paths never follow symlinks"
printf 'secret canary\n' > "$T/secret-canary"
ln -s "$T/secret-canary" "$T/linked.key"
if ./lumabri key --out "$T/linked" >"$T/link.out" 2>"$T/link.err"; then
    echo "   secret-key symlink was followed"; exit 1
fi
[ "$(cat "$T/secret-canary")" = "secret canary" ] || {
    echo "   secret-key symlink target was modified"; exit 1; }
[ "$(readlink "$T/linked.key")" = "$T/secret-canary" ] || {
    echo "   secret-key symlink itself was replaced"; exit 1; }
[ ! -e "$T/linked.pub" ] || {
    echo "   public half was left after secret-path refusal"; exit 1; }

printf 'public canary\n' > "$T/public-canary"
ln -s "$T/public-canary" "$T/public-link.pub"
if ./lumabri key --out "$T/public-link" >"$T/public-link.out" 2>"$T/public-link.err"; then
    echo "   public-key symlink was followed"; exit 1
fi
[ "$(cat "$T/public-canary")" = "public canary" ] || {
    echo "   public-key symlink target was modified"; exit 1; }
[ "$(readlink "$T/public-link.pub")" = "$T/public-canary" ] || {
    echo "   public-key symlink itself was replaced"; exit 1; }
[ ! -e "$T/public-link.key" ] || {
    echo "   orphan secret remained after public-path refusal"; exit 1; }
echo "   ✓ both symlink targets stayed intact and no orphan secret remained"

echo "LUMABRI KEY TEST: PASS"
