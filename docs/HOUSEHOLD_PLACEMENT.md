# Resident household placement

The catalogue and request path use the same resident-plan search. A successful
existing proportional plan is retained, including its node order and ranges.
This avoids gratuitously changing a working plan or its calibration identity.

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
selected computer has room for the model. Selection permits participation;
it does not force every selected computer into the chain. Only actual plan
participants are contacted during reachability preflight and receive offers.
Every participant still explicitly approves its allocation before execution.

The search is bounded and deterministic, not exhaustive and not a latency or
throughput optimizer. A failed search means no resident plan was found, not a
proof that every possible partition is impossible. It does not add computers
outside the user's selection, combine CPU RAM with VRAM, weaken the memory
guard, enable disk execution, or support multiple simultaneous sessions.

The binary search relies on the current adapters' nonnegative per-layer
resident/state costs and fixed scratch: extending a range cannot reduce its
reservation. Future contracts must preserve that property or provide another
search. Overflow never counts as a fit. Transfer metadata is recomputed for
the chosen fallback and remains an adapter estimate, not measured wire bytes.
No generation speed is inferred from this search.

## Checks

- `test_memory_budget`: legacy-plan stability, fixed-floor refusal and
  recovery, more donors than layers, source/context/session/overflow guards,
  and 500 deterministic heterogeneous budgets checked against launch costs.
- `home_flow_test.py --expect-unused-donor`: two selected donor TUIs, one below
  the process floor; only the fitting donor gets a request, approves, executes
  a real model, streams a response and releases its locks. The other donor
  receives no allocation and starts no engine.
- The existing two-donor approval, rejection, cache/calibration and node-loss
  tests continue to exercise unchanged successful plans.

These are loopback checks. Physical LAN timings and measured-cost placement
remain separate release gates.
