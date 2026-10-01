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
guard, enable disk execution, or support multiple simultaneous sessions.

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
the throughput objective. The household still admits one session: the latter
is solver behavior, not a new multi-session service.

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
