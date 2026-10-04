# Resident household placement

Every selected computer must now receive a nonempty resident layer range.
No computer outside that selection receives an offer. When a selected computer
cannot participate, change the selection or offered RAM; it is not silently
excluded. More Segment participants than layers cannot be assigned in this mode.

The memory-only search below supplies a seed, not the final household plan.
A bounded dynamic program searches cuts for all selected nodes in a fixed
order, keeping the seed's Edge owner. It independently validates all budgets.
Without stage observations, an already feasible all-selected seed stays intact.

When the proportional assignment fails the actual process budgets, the planner
tries each selected computer as Edge owner and assigns it a nonempty prefix.
The remaining computers are tried in descending and ascending RAM order, with
stable inventory-order ties. Each receives the largest contiguous range that
fits the shared resident reservation. Computers unable to hold even one layer
are skipped. The full candidate must pass the same validator used before
launch: complete coverage, one interval per participant, exactly one Edge
owner with a Segment interval, and no per-computer over-allocation.

This fixes two false refusals: rounding that leaves Edge without a layer, and
small donors that cannot pay the fixed process floor even though another
selected computer has room for the model. Such a seed may omit donors, but
the final household search must assign every selected node or refuse.
Every participant explicitly approves its allocation before execution.

The memory seed search is bounded and deterministic, not exhaustive. A failed
search means no resident plan was found, not a
proof that every possible partition is impossible. It does not add computers
outside the user's selection, combine CPU RAM with VRAM, weaken the memory
guard or enable disk execution. Conversation slots have separate runtime
admission; finding a memory fit is not a throughput guarantee.

The binary search relies on the current adapters' nonnegative per-layer
resident/state costs and fixed scratch: extending a range cannot reduce its
reservation. Future contracts must preserve that property or provide another
search. Overflow never counts as a fit. Transfer metadata is recomputed for
the chosen fallback and remains an adapter estimate, not measured wire bytes.
No generation speed is inferred from the memory search.

## Timing-guided cuts

Completed turns provide per-range RUN averages. Only matching checkpoint,
builds, hardware, node order, Edge owner, backend, threads, context and session
count allow reuse as placement costs. Duration divided by layer count is a
heuristic, including transport and queueing, not a measurement of a new range.
The search minimizes the estimated sum for one session, or maximum stage for
the throughput objective. Hosted conversation slots retain private state and
share a bounded turn queue; the solver does not imply parallel kernel execution.

Changed ranges make the old tok/s stale. The TUI labels timing-guided placement
and requires recalibration of new ranges. Requests apply the displayed snapshot
and revalidate memory; they no longer independently choose another split.
Enter during invalidated-plan refresh cannot approve an unseen plan.

## Checks

- `test_memory_budget`: legacy-plan stability, fixed-floor refusal and
  recovery, more donors than layers, source/context/session/overflow guards,
  and 500 deterministic heterogeneous budgets checked against launch costs.
- `home_flow_test.py --expect-selected-no-fit`: two selected donor TUIs, one
  below the process floor; no silent exclusion, offers, engines or source launch.
- Timing tests cover sum/max objectives, all-selected cuts, exact reservation
  limits, invalid costs and unchanged output on failure.
- The existing two-donor approval, rejection, cache/calibration and node-loss
  tests continue to exercise unchanged successful plans.

These are loopback checks. Physical LAN speed and concurrent local/remote MoE
work in an approved Hybrid allocation remain separate gates. Timing-guided
Segment cuts do not themselves parallelize a single token's layers.

## Joint resident-model preview

The planner can now evaluate several new models against the **same** remaining
RAM and allocation slots. `models --json` includes leased `workload` facts:
existing allocations, reserved bytes, local compute policy, active/waiting
kernels and a canonical allocation-set fingerprint. Unknown workload is `null`,
not an idle machine. Selecting another model tab does not change the fingerprint;
adding or releasing an allocation does. It contains no conversation text.

Advanced read-only preview (repeat model names and signed node identities):

```sh
lumabri models --tracker HOST:PORT --models-dir CHECKPOINTS --json \
  --node NODE_ID --node ANOTHER_NODE_ID \
  --together MODEL_NAME --together OTHER_MODEL_NAME --sessions 2
```

No computer is selected implicitly. Only managed donors with known local FIFO
admission are candidates. Unlike the single-model all-selected flow above,
these explicit identities define an authorized candidate pool; the preview
reports which machines each model would actually use. Up to eight models share a search over single-node
placements and descending-RAM prefixes. It independently validates every range,
then sums resident process budgets across models and checks the four-allocation
limit including existing allocations. Offered RAM is already net of existing
reservations: they are not subtracted twice. This path is CPU Segment only;
inventoried VRAM never becomes usable capacity without an adapter contract.

Where all eligible node prices are known and use one currency, candidate plans
minimize declared whole-machine hourly cost, counted once per used machine.
Otherwise the objective is resident feasibility with fewer machines, not a
fabricated zero-cost ranking. Search is limited to 20,000 states and reports
when that bound is reached. `no_candidate` means no plan in this candidate
family was found, not a proof that every possible placement is impossible.

This is **preview only**: no allocation, eviction, migration, approval or server
purchase occurs. Plans expose inventory/runtime/allocation-set identities for
revalidation. They are not executable approvals. They have no predicted tok/s
or latency guarantee. Mixed-workload calibration, requirement-aware placement,
transition costs/hysteresis remain distinct work before this becomes an
automatic global scheduler.

## Prepare selected models from the TUI

In Explore models, choose the authorized computers in the Computers tab.
Use Space on each model to select up to eight, then open `/` actions and choose
`/prepare`. The review shows each checkpoint and its exact computer/layer/RAM
assignments. Arrow keys scroll the complete joint plan. Enter starts preparation;
reviewing alone never sends an allocation offer.

The private preparation keeper loads one model at a time through the existing
authenticated approval protocol. It does not pick a different split after
review. Before the first offer, runtime, hardware, address, thread capacity and
existing allocation fingerprints must still match. Before every model, fresh
inventory must leave enough RAM and allocation slots for the remaining plan.
Each donor rechecks its own budget atomically and requires a separate approval.
Inventory is an observation, not a distributed reservation lock.

Closing the TUI does not interrupt preparation. `c` in the progress view or
Resident models cancels the unfinished work. This is a sequential operation,
**not an all-or-nothing transaction**: models already READY remain resident if
a later model is declined, fails or is cancelled. The journal reports how many
finished; their individual authenticated plans remain in Resident models.
Restarting the manager never replays old approvals or unfinished offers.
Open a conversation on each ready model from Resident models; joint preparation
does not automatically open a chat or unload another model.

Tests cover detached completion with two real tiny OLMoE checkpoints, refusal
and cancellation of the second allocation while the first still generates,
plus stale-runtime/workload/selection/budget guards. These are local integration
checks, not a mixed-model latency SLO or a hardware release claim.
