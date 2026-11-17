# Planning for speed

Splitting a model across computers does not remove work: every layer still
runs once per token, and each computer that is not the chat host adds a
network round trip. Lumabri therefore chooses a split only when no single
computer can hold the model, or when a split is estimated at least 5% faster
than the fastest single computer that can.

## What the planner does

- Every computer that can hold the whole model alone is always a candidate.
- Splits are built over at most six computers, four per split.
- A candidate whose estimate is missing a term is reported as unestimated; it
  is never guessed, and it never replaces an estimated single computer.
- When the household's memory is not enough, the request is refused with the
  reason and the shortfall.
- Admission compares two budgets, RAM and VRAM, that are never fungible.
  Every current need asks for zero VRAM: execution is CPU only.

## The estimate

For one decoded token of one active conversation:

```
seconds          = edge(E) + sum of layers(n, range) + sum of hop(E, n)
layers(n, range) = observed seconds per layer on n * layers, when a current
                   calibration of this checkpoint exists; otherwise
                   scale * active bytes(range) / segment rate(n)
edge(E)          = scale * edge resident bytes / edge rate(E)
hop(E, n)        = round trip(E, n) + 2 * boundary bytes / link rate(E, n)
tokens/s         = 1 / seconds
```

`rate` comes from a short machine probe per role, `hop` from a bounded link
probe between computers, and `scale` corrects probe-only computers once one
computer has both a probe and an observation of the checkpoint. A placement
that was measured exactly as it stands uses that measurement.

A hop whose measured round trip is above about 25 ms costs as much as a range
of layers does on a fast computer, so it sets the speed whatever the computers
are. Such a split is still planned when nothing else can hold the model, and
its reason names the link ("The link to NAME takes 60 ms per round trip...").
The same fact is in the API as `slowest_link_seconds` and `slow_link`.

The estimate is a ranking aid, not a promise. It has been checked on small
fixtures with simulated machine speeds, not on real multi-computer
households.

## Where it appears

- **Page**: in Computers & models, leave every computer unticked and review
  one model: the planner chooses among all sharing computers and the review
  shows the estimated speed and the reason. Ticking computers bounds the
  choice to them, as before. Several models together still need the
  computers to be chosen.
- **API**: `POST /api/v1/preparation` accepts an empty `nodes` list for one
  model. The reply carries `speed_plan` beside `plan`: `status`, `selection`,
  `chosen`, `candidates`, `best_split`, `computers_that_fit_alone`, `why` and
  `reason`. A refusal with automatic selection answers `review: null` and the
  reason.
- **Terminal**: the model screen shows the estimate and how many computers
  the chosen placement uses.
- **Command line**: `lumabri models --tracker H:P --speed-plan NAME
  [--node ID ...] [--current ID:B-E,...] [--json]`.

## Re-planning

When a computer joins or leaves, a better placement is only proposed, and
only when it is estimated at least 10% faster than the current one. Nothing
is unloaded or loaded without the same approval as any other preparation.

## Not covered

- GPU execution and the VRAM budget's use.
- Sampling changes for Qwen3.8 and multi-row V4.1.
- Long prompts in a chain and several conversations in parallel are priced
  through the configured number of sessions; no new mechanism was added.
