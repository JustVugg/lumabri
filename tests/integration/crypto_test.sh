#!/usr/bin/env bash
# lumabri — the transport primitives against their published test vectors.
#
# ChaCha20, Poly1305 and the AEAD from RFC 8439, X25519 from RFC 7748,
# HMAC-SHA512 from RFC 4231, and an independently generated HKDF-SHA512
# vector using RFC 5869's first test-case inputs.
# A cipher that merely round-trips proves nothing; a cipher that reproduces
# the RFC's own bytes is the real check, the same standard sign_test holds
# Ed25519 to. Also a tamper case: one flipped byte must fail the tag.
set -euo pipefail
cd "$(dirname "$0")/../.."

T=$(mktemp -d /tmp/lumabri-crypto.XXXXXX)
trap 'rm -rf "$T"' EXIT

cc -O2 -Wall -Wextra -Werror -Wno-unused-function -I. tests/c/test_crypto_vectors.c -o "$T/t"
"$T/t"
