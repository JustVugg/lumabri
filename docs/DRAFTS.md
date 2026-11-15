# Drafts across the segments

Splitting a model's layers across machines adds one network round to every
token: the token's row leaves the Edge, crosses every segment and comes back.
A draft removes rounds without changing the reply. A source proposes the next
few tokens, the whole chain runs all of them as **one block of rows in one
round**, and the Edge keeps exactly the rows whose proposal turned out to be
the token the model selects anyway.

## What is implemented

| Piece | State |
|---|---|
| Multi-row verification along the chain, one round per block | implemented |
| Commit and rollback per row in every segment | implemented for OLMoE (KV) and Qwen3.8 (KV plus one copy of the recurrent state per row) |
| Prompt lookup, the source that needs no weights | implemented, every family whose segments can roll back |
| Capability negotiation and fallback with older segments | implemented |
| Observations and an explicit setting | implemented, default off |
| Qwen3.8 MTP head as a source | **not implemented**; design below |
| DeepSeek V4.1 (any source, DSpark included) | **not possible on today's V4.1 chain**; reason and design below |
| Chained blocks (pipelining) | **not implemented**; design below |

## One round

The Edge holds the last selected token `t`, which the chain has not run yet,
and proposals `d1 … dk`. It sends the rows `[t, d1, … dk]` at the position of
`t`. Row 0 is certain. Row `i` is what the model computes after `t, d1 … di`,
so it is the true next state exactly while `d1 … di` were the tokens really
selected. The Edge selects from row 0, compares the result with `d1`, and only
then selects from row 1, and so on; it stops at the first proposal that was
wrong, at the end token, or at the token limit.

- **Greedy and sampled replies are identical to decode without drafts.**
  Selection runs once per kept token, in the same order, on a state computed
  from the same tokens. With sampling the sampler therefore draws the same
  numbers for the same seed; a draft never consumes one.
- A wrong proposal costs compute, not correctness: its row, and the rows
  after it, are discarded.

The gain is network rounds. On a CPU every row of a block still costs every
segment one row of compute, and a rejected row is compute thrown away.

## Rollback in a segment

`LMB_SEG_DRAFT` (115/116) carries the ordinary `SEG2` run envelope. Its rows
are *provisional*: the next request names the position generation continues
from, and the segment discards its rows at and above that position before it
runs anything. The discard therefore travels with the next block and costs no
round of its own. A draft with no rows only discards; the chatter sends one
when a reply ends inside a block (an end token), so the checkpoint and the
next turn's prefill find every segment at the committed position.

Bounds the session table enforces before an engine is touched:

- a draft may start anywhere from the **second** row of the previous committed
  draft up to the current position. The first row is certain and stays; rows
  of any earlier request are final;
- a plain `SEG_RUN`, a snapshot or a restore never moves a position backwards
  and closes the window;
- sequence numbers, request digests, duplicate recognition and fencing are
  those of `SEG_RUN`. A retried draft is answered from the cached reply, never
  run twice;
- if the engine fails after it has discarded rows, the session demands a
  restore: the table no longer trusts its position, and the chatter's existing
  recovery (fresh sessions, checkpoint, replay of committed tokens) takes over.

Engine side (`engine_patches/segment_draft.h`, applied to the build copy of the
engine by `tools/prepare_draft_rows.py`; the upstream checkout is untouched):

- **OLMoE.** Attention reads KV rows below the session position only, so
  forgetting rows is the whole rollback.
- **Qwen3.8.** KV and index rows roll back the same way. DeltaNet, its
  convolution and the PLE convolution are recurrent and cannot be computed
  backwards, so the segment keeps one copy of that state after every row of a
  block but the last. Rolling back to a row restores its copy. Cost: `rows - 1`
  copies of the recurrent state per conversation, allocated on first use.
  These copies are **not yet priced** in the segment's session reservation;
  that has to be added to the planner before drafts are enabled by default.
- Both run a provisional block **one row at a time**, with the forward a
  decode step uses. Every kept row is therefore bit for bit the row plain
  decode computes; a batched forward would be a different kernel and could
  differ in the last bits. The saving is network rounds, not compute.
- Families without a rollback (GLM, GLM-5.3, Inkling, Kimi, Qwen3.6, DeepSeek
  V4 and V4.1) do not offer provisional rows and decode as before.

## Sources

**Prompt lookup** re-proposes what followed the most recent earlier occurrence
of the last 2–4 tokens of the history (Colibri's `spec_lookup`). It needs no
weights and no memory beyond the token history the Edge already holds. It pays
on quoted text, code edits and repeated structure; on free text it proposes
little.

**Qwen3.8 MTP head (design, not implemented).** In Colibri the head is one
extra layer (index `num_layers`) with its own KV. It is fed the final hidden
row of every position together with the next token (`q38_mtp_feed`) and
drafts from there (`q38_mtp_draft`, `q38_mtp_draft_more`). The Edge already
receives every final row, prefill included, so the head belongs at the Edge:

1. load the head's layer and attach it in the Qwen3.8 Edge adapter, behind a
   reserved Edge entry point, as `tools/prepare_draft_rows.py` does for the
   segments;
2. keep one head state per conversation (its KV rows and pending streams) and
   roll it back with the block;
3. price the head in the Edge's resident reservation (`planner_adapters`),
   since weights load before READY and nothing may be read later;
4. extend the reduced fixture with head tensors. A random head on a random
   model accepts almost nothing, so the fixture gate needs trained-like
   agreement or keeps using scripted drafts for the round count.

The chain side needs nothing new: proposals are token ids, and rows are
verified and rolled back exactly as for lookup.

**DeepSeek V4.1 (not possible today).** Two independent obstacles:

- The production V4.1 chain feeds the committed state of its **tail back to
  its head for the next token** (compressed-KV rows, the published index
  owner, shared top-k and candidate blocks; see `DEEPSEEK_V41.md`). Row `i+1`
  at the first segment needs what the last segment produced for row `i`, so a
  block cannot cross the chain in one traversal. This is why the adapter is
  single-row. Lifting it means letting every segment own and carry forward the
  shared state for a whole block, with a boundary that carries one delta per
  row; that is a new state schema and a new range oracle, not a change to
  drafts.
- DSpark reads the hidden states of several target layers
  (`dspark_target_layer_ids`), not only the last one. With layers on other
  machines those rows are not at the Edge; the boundary would have to carry
  them.

Until both are solved a V4.1 household runs without drafts, whatever the
setting says.

## Chained blocks (design, not implemented)

While segment 2 verifies block *n*, segment 1 could already run block *n+1*.
Block *n+1* must be proposed before the result of block *n* is known, so it is
built on the assumption that block *n* is accepted in full. That is sound with
the rollback above — a wrong assumption discards block *n+1* everywhere — but
it needs: a second provisional window per session (the table allows one),
requests in flight on one session (the table allows one), a draft source that
can extend its own unverified proposals, and cancellation of in-flight rows.
It only pays when acceptance is high and the chain is long. It was left out
rather than added on top of sources that are not measured on real checkpoints.

## Compatibility

A segment says in `SEG_OPEN_R` (`flags` bit 0) that it accepts provisional
rows. The chatter drafts only when **every** segment of the chain said so; an
older segment never sets the bit and is never sent the new operation, an older
chatter ignores the bit, and the whole chain then decodes one row per round as
before. Relayed segments work when the tracker forwards `LMB_SEG_DRAFT`; an
older tracker does not, and the relay failure is handled by the existing
recovery path. See `PROTOCOL_COMPATIBILITY.md`.

## Setting

Off by default: whether rounds saved outweigh rows wasted depends on the
household's links and on the reply, and that decision belongs to the planner
once it is measured.

| | |
|---|---|
| `--drafts off\|lookup\|head\|auto` or `LUMABRI_DRAFTS` | `auto` uses the family's draft head where one exists, then prompt lookup; today `auto` is prompt lookup and `head` proposes nothing |
| `--draft-rows 2..16` or `LUMABRI_DRAFT_BLOCK` | rows per block, the certain row included (default 5) |
| `LUMABRI_DRAFT_ROWS=0` on a segment | the segment does not offer provisional rows |

`LUMABRI_DRAFT_SCRIPT` (a list of token ids, entry *i* proposed for reply
token *i*) is a test instrument: it lets a gate drive commit and rollback with
a chosen mix of right and wrong proposals. It cannot change a reply.

## Observations

A reply that used at least one draft round reports, next to the generation
metrics (`GENERATION_METRICS.md`):

- `rounds`: chain traversals during decode, discard-only ones included;
- `tokens`: tokens those rounds selected (`decode_steps`);
- `proposed` / `accepted`: draft rows sent, and draft rows whose output
  selected a token;
- `discards`: rounds that only took rows back.

`rounds / tokens` is the network cost per token; without drafts it is 1.
Per-segment stage timing (`STAGES1`, link evidence) is omitted for such a
reply: a block is not a one-row decode step, and mixing them would corrupt the
calibration that reads it.

## Verification

`make test-drafts ENGINE=…` (also part of `make test` and Linux CI) runs a
real tracker, two real segments per family and the chatter on loopback, on
reduced random fixtures:

- OLMoE and Qwen3.8, two segments each: greedy replies with drafts equal
  replies without, token for token; seeded sampled replies too (OLMoE; the
  Qwen3.8 Edge has no logits to sample from);
- rounds per token below one (measured: OLMoE prompt lookup 23 rounds for 39
  tokens; scripted drafts with one proposal in six wrong, 12 rounds for 39
  tokens on both families);
- every gate rejects drafts, so rollback runs; on Qwen3.8 that is the
  per-row recurrent state;
- a resident conversation whose reply ends inside a block discards the
  leftover rows and the next turn reuses the resident state;
- a chain with one segment that does not offer provisional rows decodes
  without drafts.

Not verified: any real checkpoint, acceptance on real text, wall-clock gain,
relay transport, failover in the middle of a block (it takes the existing
recovery path, which the failover gates cover without drafts).
