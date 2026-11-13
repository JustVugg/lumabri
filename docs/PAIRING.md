# Adding a computer with a short code

The owner of a household asks for a code, reads eight characters such as
`7K3M-9QXT` to the person at the other computer, and that computer joins. The
32-character household key is never typed. Joining with the full key still
works exactly as before.

| where | owner's computer | other computer |
|---|---|---|
| command | `lumabri pair` | `lumabri join --code CODE [HOST:PORT]` |
| workspace | `/pair` (Add a computer) | `/join`, then the code |
| page | Computers > Add a computer | Computers > I have a code |

## Threat model

The attacker is on the same network. It can read, drop, change and inject
every packet, answer LAN discovery with a household of its own, and open as
many connections as it likes. It cannot see the owner's screen and does not
hold either computer's identity key or the household key.

A code has 40 bits. It is therefore never used as a key, as a MAC key, or as
input to anything an attacker could recompute offline after watching or
disturbing an exchange. It only selects the base point of a Diffie-Hellman
exchange (a balanced PAKE), so:

- watching an exchange gives nothing to test a guess against;
- taking part in one, as either side or in the middle, tests exactly one guess;
- the tracker allows five such tests per code, then destroys it.

## The exchange

It runs inside the usual encrypted connection (X25519 and ChaCha20-Poly1305,
signed by each side's Ed25519 identity), opened without consulting or learning
a pin, on new operations `LMB_PAIR_HELLO/HELLO_R/CONFIRM/DONE` (106 to 109).
`id_T` and `id_J` are the identities that signed that connection.

1. Joiner to tracker, before `AUTH`: version, 16 random bytes `n_J`.
2. Tracker: 16 random bytes `n_T`; `sid = n_T | n_J`. It takes the live code,
   or a random decoy when no code may be tested (see Limits), and computes
   `g = Elligator2(SHA-512(lv("CPace255") | lv(code) | lv(padding) | lv(CI) | lv(sid))[0..31])`
   with `CI = lv("lumabri-pair-v1") | lv(id_T) | lv(id_J)`, then
   `Y_T = X25519(y_T, g)` for a fresh `y_T`. It sends version, `n_T`, `Y_T`.
3. Joiner: the same `g` from the typed code, `Y_J = X25519(y_J, g)`,
   `K = X25519(y_J, Y_T)`, refused if `K` is all zero. Then
   `ISK = SHA-512(lv("CPace255_ISK") | lv(sid) | lv(K) | lv(Y_T) | lv(id_T) | lv(Y_J) | lv(id_J))`,
   `th = SHA-512(lv("lumabri-pair-v1 transcript") | lv(sid) | lv(id_T) | lv(id_J) | lv(Y_T) | lv(Y_J))`,
   and three keys `HKDF-SHA512(salt "lumabri-pair-v1", ISK, label)[0..31]` for the
   labels `confirm joiner`, `confirm tracker` and `household key`. It sends
   `Y_J` and `tag_J = HMAC-SHA512(key(confirm joiner), th)[0..31]`.
4. Tracker: the same derivation. It compares `tag_J` in constant time. Only
   if it matches, and the code is still live, does it mark the code used and
   send `tag_T = HMAC-SHA512(key(confirm tracker), th)[0..31]` followed by the
   household key and `id_T`, sealed with ChaCha20-Poly1305 under
   `key(household key)` (zero nonce, used once; `th` as associated data; the
   plaintext has a fixed length).
5. Joiner: checks `tag_T` before decrypting, checks that the sealed identity
   is `id_T`, then pins `id_T` for the address (the operator's pin file or
   `known_hosts`, under their usual rules), opens a normal pinned connection,
   authenticates with the key, and only then saves `home.conf`.

`lv(x)` is the LEB128 length of `x` followed by `x`. Because both identities
enter `g` and `ISK`, a relay that terminates the encrypted connection on each
side makes the two sides derive different points, whatever it knows.

## Limits at the tracker

- One live code per household. A new one destroys the previous one.
- Lifetime 300 seconds. `LUMABRI_PAIR_LIFETIME_S` can shorten it for tests;
  larger values are ignored by the client and capped by the tracker.
- Five exchanges per code, counted when they start. A wrong tag, a malformed
  or missing confirmation, and a dropped connection are all failures. The
  fifth failure destroys the code and the owner is told.
- One use. The first proven exchange consumes the code before anything is sent.
- Per source address: five attempts at once, then one every two seconds.
- An attempt that may not test the code (none live, limit reached, over the
  rate) runs the same exchange against a random decoy. Every refusal is the
  same `pairing failed`, sent 300 ms after the confirmation frame arrived.
- A code is drawn from the system random generator, five bits per symbol
  (Crockford base 32, no I L O U), with no rejection and no modulo bias. It
  exists in tracker memory and in the reply to the owner. It is not logged or
  written to disk and is wiped when it stops being live.
- Codes are created, watched and withdrawn only over a connection that sent
  the household key and carries the tracker's own identity.

## What an attacker on the LAN can and cannot do

Cannot: learn the code, the household key or anything testable by listening;
test more than one guess per exchange it joins or relays; test more than five
guesses against one code (five chances in 2^40); make the joiner pin or save
anything for a computer that did not prove knowledge of the code; replay a
recorded exchange; use a low-order point.

Can: make pairing fail, by dropping packets or by spending the five attempts
(the owner sees the count, and the address they came from); see that a
pairing happened and which addresses took part; learn, as today, both
computers' public identity keys.

Deliberate trade-offs: the joiner cannot tell a wrong code from an expired,
used or rate-limited one; an address already pinned to a different identity
is not overruled by a successful exchange (the join stops, the code is
spent); joining from the page saves the new settings and then restarts the
local services with `lumabri service stop` and `lumabri start`.

## The page

Loopback gateway only, bearer sign-in with the management permission:

- `GET /api/v1/household` returns `{tracker, owner}`.
- `POST /api/v1/household/pairing` with `{"action":"create"}` returns
  `{id, code, expires_in, max_attempts, address}`; `{"action":"cancel","id"}` withdraws.
- `GET /api/v1/household/pairing/ID` returns `{state, expires_in, failed, max_attempts, joiner, source}`.
- `POST /api/v1/household/join` with `{"code", "address"?, "ram_gb"?}` runs the
  exchange; on success it answers `202 {joined, tracker, restarting}`.

## Compatibility

A tracker without these operations answers `ERR` and closes; the joiner says
so and the full key remains. Bodies have one exact length and version 1.
The Windows workspace does not offer the code yet; `lumabri` on Windows can
still join with the full key, and a Windows tracker serves codes.

## Open item: the specification's test vectors

The base-point and session-key formulas follow the IRTF CFRG draft "CPace, a
balanced composable PAKE" (`draft-irtf-cfrg-cpace`), suite
CPACE-X25519-SHA512, with Elligator 2 as in RFC 9380 section 6.7.1. They were
written without network access and **have not been run against the published
vectors, so no conformance is claimed.** The confirmation tags and the sealed
message are Lumabri's own layer, not the draft's.

What has been checked: Elligator 2 output is a canonical u-coordinate on the
curve; the C code agrees with an independent big-integer model on the map, on
whole exchanges in both directions and on low-order values
(`tests/integration/pairing_model_test.py`); wrong codes, relays, replays,
tampered values, truncated frames and the limits are refused
(`tests/c/test_pairing.c`, `tests/c/test_pairing_tracker.c`,
`tests/integration/pairing_test.py`). The model was written from the same
reading of the draft, so it cannot catch a misreading.

To close the item, someone with network access should check, in the draft's
appendix "Test vectors", the section "Test vector for CPace using group X25519
and hash SHA-512" (B.1 in the revisions known to the author: generator string
and `calculate_generator`, both messages, the secret point `K`, the `ISK`
calculation for initiator and responder, and the low-order points for
`scalar_mult_vfy`), and the `curve25519_XMD:SHA-512_ELL2_NU_` vectors of RFC
9380 Appendix J for the map. `./test_pairing map HEX` prints the map for one
input.
