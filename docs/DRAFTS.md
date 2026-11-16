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
  The copies live inside the session's existing memory limit: at OPEN the
  segment asks the adapter how many rows a block may carry so that the
  session state plus those copies still fit, offers exactly that many (0
  means no drafts for this session), and the adapter refuses a larger block
  before touching state. OLMoE's rollback needs no memory. The chatter uses
  the smallest offer along the chain.
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
extra transformer layer (`blk.<num_layers>`, with its own attention, experts
and `nextn.eh_proj`) that shares the embedding and the output head. It is fed
the final streams of every position paired with the token after it
(`q38_mtp_feed`), keeps its own KV rows for verified pairs only, and drafts
with `q38_mtp_draft` / `q38_mtp_draft_more`. The Edge already receives every
final row, prefill included, so the head belongs at the Edge. What stands
between the engine's implementation and a household:

1. `q38_mtp_attach` refuses unless the **whole model** is loaded in one
   process (`range_begin == 0 && range_end == layers` and a resident output
   head). The Edge holds no layer. The build-copy patch has to open the Edge's
   model with the range `[layers, layers + 1)` so that only the head's layer
   loads, and lift that check for this case;
2. the head's state lives in the engine's single `Model` (`K/V/IK[layers]`,
   `mtp_len`, `mtp_pend`). A resident Edge serves several conversations, so
   that state must become per conversation, swapped in around each call as
   the Segment adapters do for their sessions, and must roll back with the
   block. `q38_mtp_feed` already tolerates a rewind: rows above the new
   position are dropped;
3. the Edge ABI has no call for "these rows were committed" or "propose";
   both need reserved Edge entry points, as `segment_draft.h` does for the
   Segment side;
4. the head's layer (attention, all its experts, `eh_proj`) and its KV per
   conversation must be priced in the Edge's resident reservation and loaded
   before READY (`planner_adapters`, the GGUF inspector, which today refuses
   unknown tensors);
5. the reduced fixture needs `blk.<layers>.*` tensors. The generator in
   `tests/integration/qwen38_native_gguf_test.py` is numpy only and can be
   extended without torch. A random head on a random model accepts almost
   nothing, so its gate proves identity under rejection; the round count
   still needs scripted drafts or a fixture built to agree.

The chain side needs nothing new: proposals are token ids, and rows are
verified and rolled back exactly as for lookup. This is the largest remaining
piece: an engine patch of a few hundred lines against the least stable part
of the Qwen3.8 source, plus the planner and inspector contracts.

**DeepSeek V4.1 (not possible today; design).** Two independent obstacles.

*1. The chain's feedback.* Every V4.1 boundary row carries, beside the mHC
streams, an auxiliary frame (`engine_patches/v41_boundary.h`): for every
compressed-KV owner layer the newest closed row of its compressed K/V and
index key, the published index owner, the candidate block mask and the shared
top-k indices. Layers consume state published by owners **on other machines**,
including owners later in the chain: the committed frame of the tail is fed
back into the head's input for the next token (`chain_run_observed`). Row
`i+1` at the first segment therefore needs what the last segment produced for
row `i`, and a block cannot cross the chain in one traversal.

What has to change so it can:

- Classify each published item by direction. State published by an *earlier*
  layer and read by a later one can travel forward inside the block: the
  boundary carries one auxiliary frame **per row** instead of one. This is a
  wider frame, not a new idea.
- State published by a *later* layer and read by an earlier one on the next
  token is the real obstacle. For compressed KV it only changes when a row
  closes, once every `compress_ratio` tokens. A block can cross in one
  traversal exactly when no backward-read item changes inside it; the chatter
  can compute that from positions and ratios and cut blocks at those
  boundaries. Whether the published index owner, candidate mask and shared
  top-k are read backwards per token has to be established from the engine
  (`deepseek_v41.c` forward, not from the boundary codec). If they are, drafts
  are limited to what one segment can verify alone, which is no gain.
- Each segment then needs its foreign-owner state per row, with rollback:
  compressed-KV counts and rows are position-indexed and truncate like KV;
  candidate masks and top-k are per token and must be kept per row, as
  Qwen3.8's recurrent state is.
- A new state schema (`lmb-v41-text-delta2`), numeric class and range oracle:
  the existing `test-v41-range` proves exact logits for one row per round at
  every cut; the same proof is needed for blocks, including a block that
  straddles a compressed-row boundary being refused or split.

*2. DSpark's inputs.* DSpark reads the hidden states of several target layers
(`dspark_target_layer_ids`) and has its own experts and a confidence output
that Colibri's gate uses to choose the depth. Options: carry the target
layers' rows to the Edge inside the boundary (wider by one hidden row per
target layer not in the last segment), or run DSpark in the last segment and
return its proposals and confidences in a reply extension, which requires all
target layers to be in that segment or forwarded to it. The second keeps the
Edge small and is preferable; it needs a draft-source reply on `SEG_DRAFT_R`.

Size and risk: the schema, per-row frames, rollback and oracle are comparable
to the original V4.1 adapter work; DSpark is a second piece of similar size
(loader, memory pricing, Engram interaction, fixtures with a DSpark head).
The main risk is the backward-read question above: if it holds per token,
part 1 does not pay and only DSpark-free plain decode remains. Until both are
solved a V4.1 household runs without drafts, whatever the setting says.

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
rows, and in bits 8–15 how many rows one block of this session may hold. The chatter drafts only when **every** segment of the chain said so; an
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
Per-segment timing keeps flowing. A round that carried only the certain row
is an ordinary one-row decode step and is timed as one; `STAGES1` then reports
those rounds (its call count is below `decode_steps`, and equal for every
range) and `LINKS1` is emitted as usual. Blocks are not timed as steps: their
time covers several rows and would distort the per-step calibration. A reply
made only of blocks has no stage observation. The recorded decode rate of a
drafted reply is the rate it really had, drafts included; the calibration key
does not yet say whether drafts were on.

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
