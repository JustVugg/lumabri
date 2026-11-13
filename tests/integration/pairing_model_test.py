"""The pairing exchange in C against an independent big-integer model.

Everything here is written from the formulas, with Python integers and the
standard library, and shares no code with the C implementation: the field is
`int % P`, Elligator 2 is the textbook map with an explicit square root, X25519
is the ladder of RFC 7748, and the AEAD is RFC 8439 spelled out.

What agreement proves: the C code computes the construction documented in
src/crypto/lumabri_cpace.h. What it does not prove: that this construction is
byte for byte the CPace draft. Both sides were written from the same reading of
it, without its test vectors. See docs/PAIRING.md, "Open item".
"""
import hashlib
import hmac
import os
from pathlib import Path
import secrets
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "test_pairing"

P = 2**255 - 19
A = 486662
ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
LABEL = b"lumabri-pair-v1"
SECRET_LEN = 2 + 127 + 32
LOW_ORDER = [0, 1, P - 1, P, P + 1,
             325606250916557431795983626356110631294008115727848805560023387167927233504,
             39382357235489614581723060781553021112529911719440698176882885853963445705823]


# ---- the field and the curve ------------------------------------------------
def is_square(x):
    return pow(x % P, (P - 1) // 2, P) in (0, 1)


def square_root(x):
    """A root of x modulo P = 5 (mod 8), or None."""
    x %= P
    root = pow(x, (P + 3) // 8, P)
    if root * root % P != x:
        root = root * pow(2, (P - 1) // 4, P) % P
    return root if root * root % P == x else None


def curve_rhs(u):
    return (u**3 + A * u**2 + u) % P


def elligator2(data):
    """RFC 9380 section 6.7.1 with J = A, K = 1 and Z = 2; the u-coordinate only."""
    r = (int.from_bytes(data, "little") & (2**255 - 1)) % P
    denominator = (1 + 2 * r * r) % P
    x1 = -A * pow(denominator, P - 2, P) % P      # 0 when the denominator is 0
    if x1 == 0:
        x1 = -A % P
    x2 = (-x1 - A) % P
    x = x1 if is_square(curve_rhs(x1)) else x2
    assert square_root(curve_rhs(x)) is not None, "Elligator 2 left the curve"
    return x.to_bytes(32, "little")


def x25519(scalar, point):
    """RFC 7748 section 5."""
    k = int.from_bytes(scalar, "little")
    k &= ~7 & (2**255 - 1)
    k |= 2**254
    u = (int.from_bytes(point, "little") & (2**255 - 1)) % P
    x_2, z_2, x_3, z_3, swap = 1, 0, u, 1, 0
    for t in range(254, -1, -1):
        bit = (k >> t) & 1
        if swap ^ bit:
            x_2, x_3, z_2, z_3 = x_3, x_2, z_3, z_2
        swap = bit
        a, b = (x_2 + z_2) % P, (x_2 - z_2) % P
        aa, bb = a * a % P, b * b % P
        e = (aa - bb) % P
        c, d = (x_3 + z_3) % P, (x_3 - z_3) % P
        da, cb = d * a % P, c * b % P
        x_3 = (da + cb) ** 2 % P
        z_3 = u * (da - cb) ** 2 % P
        x_2 = aa * bb % P
        z_2 = e * (aa + 121665 * e) % P
    if swap:
        x_2, z_2 = x_3, z_3
    return (x_2 * pow(z_2, P - 2, P) % P).to_bytes(32, "little")


# ---- the exchange -----------------------------------------------------------
def lv(data):
    """Unsigned LEB128 length, then the bytes."""
    n, prefix = len(data), bytearray()
    while True:
        low, n = n & 0x7F, n >> 7
        prefix.append(low | (0x80 if n else 0))
        if not n:
            return bytes(prefix) + data


def generator(code, id_tracker, id_joiner, sid):
    prs, dsi = code.encode(), b"CPace255"
    ci = lv(LABEL) + lv(id_tracker) + lv(id_joiner)
    padding = bytes(128 - 1 - len(lv(prs)) - len(lv(dsi)))
    digest = hashlib.sha512(lv(dsi) + lv(prs) + lv(padding) + lv(ci) + lv(sid)).digest()
    return elligator2(digest[:32])


def hkdf(ikm, info):
    prk = hmac.new(LABEL, ikm, hashlib.sha512).digest()
    return hmac.new(prk, info + b"\x01", hashlib.sha512).digest()[:32]


def derive(shared, id_tracker, id_joiner, sid, y_tracker, y_joiner):
    isk = hashlib.sha512(lv(b"CPace255_ISK") + lv(sid) + lv(shared) + lv(y_tracker) + lv(id_tracker) +
                         lv(y_joiner) + lv(id_joiner)).digest()
    transcript = hashlib.sha512(lv(LABEL + b" transcript") + lv(sid) + lv(id_tracker) + lv(id_joiner) +
                                lv(y_tracker) + lv(y_joiner)).digest()
    tag = lambda label: hmac.new(hkdf(isk, label), transcript, hashlib.sha512).digest()[:32]
    return {"tag_tracker": tag(b"confirm tracker"), "tag_joiner": tag(b"confirm joiner"),
            "seal_key": hkdf(isk, b"household key"), "transcript": transcript}


# ---- ChaCha20-Poly1305, RFC 8439 -------------------------------------------
def chacha_block(key, counter, nonce):
    def rotl(v, n):
        return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF
    state = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    state += [int.from_bytes(key[i:i + 4], "little") for i in range(0, 32, 4)]
    state += [counter] + [int.from_bytes(nonce[i:i + 4], "little") for i in range(0, 12, 4)]
    x = list(state)

    def quarter(a, b, c, d):
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = rotl(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = rotl(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = rotl(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = rotl(x[b] ^ x[c], 7)
    for _ in range(10):
        quarter(0, 4, 8, 12); quarter(1, 5, 9, 13); quarter(2, 6, 10, 14); quarter(3, 7, 11, 15)
        quarter(0, 5, 10, 15); quarter(1, 6, 11, 12); quarter(2, 7, 8, 13); quarter(3, 4, 9, 14)
    return b"".join(((x[i] + state[i]) & 0xFFFFFFFF).to_bytes(4, "little") for i in range(16))


def chacha(key, nonce, data):
    stream = b"".join(chacha_block(key, 1 + i, nonce) for i in range((len(data) + 63) // 64))
    return bytes(a ^ b for a, b in zip(data, stream))


def poly1305(key, message):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s, acc = int.from_bytes(key[16:32], "little"), 0
    for i in range(0, len(message), 16):
        acc = (acc + int.from_bytes(message[i:i + 16] + b"\x01", "little")) * r % (2**130 - 5)
    return ((acc + s) % 2**128).to_bytes(16, "little")


def aead_tag(key, nonce, aad, ciphertext):
    pad = lambda data: bytes(-len(data) % 16)
    body = aad + pad(aad) + ciphertext + pad(ciphertext) + len(aad).to_bytes(8, "little") + \
        len(ciphertext).to_bytes(8, "little")
    return poly1305(chacha_block(key, 0, nonce)[:32], body)


def seal(keys, plaintext):
    ciphertext = chacha(keys["seal_key"], bytes(12), plaintext)
    return ciphertext + aead_tag(keys["seal_key"], bytes(12), keys["transcript"], ciphertext)


def unseal(keys, sealed):
    ciphertext, tag = sealed[:-16], sealed[-16:]
    if not hmac.compare_digest(tag, aead_tag(keys["seal_key"], bytes(12), keys["transcript"], ciphertext)):
        return None
    return chacha(keys["seal_key"], bytes(12), ciphertext)


def encode_secret(key, id_tracker):
    return bytes([1, len(key)]) + key.encode().ljust(127, b"\0") + id_tracker


# ---- the C side -------------------------------------------------------------
def c_side(*args):
    done = subprocess.run([str(BINARY), *args], capture_output=True, text=True, timeout=20)
    assert done.returncode == 0, (args[0], done.returncode, done.stderr)
    return dict(line.split(" ", 1) for line in done.stdout.splitlines())


def main():
    assert BINARY.exists(), "build it first: make test_pairing"
    # The model's own primitives against the published vectors they come from.
    assert x25519(bytes.fromhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                  bytes.fromhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")).hex() == \
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"      # RFC 7748 5.2
    assert poly1305(bytes.fromhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"),
                    b"Cryptographic Forum Research Group").hex() == "a8061dc1305136c6c22b8baf0c0127a9"  # RFC 8439 2.5.2
    assert lv(b"") == b"\0" and lv(bytes(127))[:1] == b"\x7f" and lv(bytes(128))[:2] == b"\x80\x01"
    assert not is_square(2) and is_square(P - 1) and (A * A - 4) % P and not is_square(A * A - 4)

    # Elligator 2: edge values, then random ones. The model asserts that each
    # result is on the curve by producing the square root.
    edges = [0, 1, 2, P - 1, P, P + 1, 2**255 - 1, 2**255, 2**256 - 1, (P - 1) // 2, (P + 1) // 2]
    inputs = [value.to_bytes(32, "little") for value in edges] + [os.urandom(32) for _ in range(300)]
    points = set()
    for data in inputs:
        expected = elligator2(data)
        assert c_side("map", data.hex())["u"] == expected.hex(), data.hex()
        points.add(expected)
    assert len(points) > 300    # not a constant; -r and r collide only by chance

    def code():
        return "".join(secrets.choice(ALPHABET) for _ in range(8))

    for _ in range(24):
        id_tracker, id_joiner, sid = os.urandom(32), os.urandom(32), os.urandom(32)
        scalar_tracker, scalar_joiner = os.urandom(32), os.urandom(32)
        shared_code, key = code(), secrets.token_hex(16)
        common = [shared_code, id_tracker.hex(), id_joiner.hex(), sid.hex()]
        base = generator(shared_code, id_tracker, id_joiner, sid)
        y_tracker, y_joiner = x25519(scalar_tracker, base), x25519(scalar_joiner, base)
        shared = x25519(scalar_tracker, y_joiner)
        assert shared == x25519(scalar_joiner, y_tracker) and any(shared)
        model = derive(shared, id_tracker, id_joiner, sid, y_tracker, y_joiner)

        # The C tracker against the model's joiner.
        assert c_side("begin", "tracker", *common, scalar_tracker.hex())["public"] == y_tracker.hex()
        tracker = c_side("finish", "tracker", *common, scalar_tracker.hex(), y_joiner.hex(), key)
        assert tracker["public"] == y_tracker.hex() and tracker["low_order"] == "0"
        for name, value in model.items():
            assert tracker[name] == value.hex(), name
        opened = unseal(model, bytes.fromhex(tracker["sealed"]))
        assert opened == encode_secret(key, id_tracker)

        # The model's tracker against the C joiner.
        joiner = c_side("finish", "joiner", *common, scalar_joiner.hex(), y_tracker.hex(),
                        seal(model, encode_secret(key, id_tracker)).hex())
        assert joiner["public"] == y_joiner.hex()
        for name, value in model.items():
            assert joiner[name] == value.hex(), name
        assert joiner["opened"] == "1" and joiner["key"] == key and joiner["identity"] == id_tracker.hex()

        # One wrong symbol on the C joiner: neither proof matches, nothing opens.
        wrong = shared_code[:-1] + ("0" if shared_code[-1] != "0" else "1")
        refused = c_side("finish", "joiner", wrong, *common[1:], scalar_joiner.hex(), y_tracker.hex(),
                         seal(model, encode_secret(key, id_tracker)).hex())
        assert refused["opened"] == "0" and refused["public"] != y_joiner.hex()
        assert refused["tag_joiner"] != model["tag_joiner"].hex()
        assert refused["tag_tracker"] != model["tag_tracker"].hex()
        # A tampered sealed secret under the right keys.
        damaged = bytearray(seal(model, encode_secret(key, id_tracker))); damaged[5] ^= 1
        assert c_side("finish", "joiner", *common, scalar_joiner.hex(), y_tracker.hex(),
                      damaged.hex())["opened"] == "0"

    # Low-order values: zero in the model, refused by both C roles.
    for value in LOW_ORDER:
        point = value.to_bytes(32, "little")
        assert not any(x25519(os.urandom(32), point)), value
        for role in ("tracker", "joiner"):
            answer = c_side("finish", role, code(), os.urandom(32).hex(), os.urandom(32).hex(),
                            os.urandom(32).hex(), os.urandom(32).hex(), point.hex())
            assert answer["low_order"] == "1" and "seal_key" not in answer
    print("pairing model: PASS (Elligator 2 on 311 inputs, 24 exchanges in both directions, "
          "low-order values; not a CPace conformance test)")


if __name__ == "__main__":
    main()
