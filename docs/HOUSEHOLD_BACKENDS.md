# Household execution backend contract

The current household planner reserves CPU RAM, not GPU memory. Approval must
therefore request CPU execution explicitly in **both** Colibri engine opens.
`backend_mask=0` is not CPU: the public ABI defines it as the adapter's automatic
local policy, which can change independently of Lumabri's memory planner.

The approved donor launcher sets `LUMABRI_ENGINE_BACKEND=cpu`, overriding any
inherited preference. Segment and Edge pass the CPU mask before loading and
check the returned backend flags before advertising readiness. A mismatched,
missing or mixed CPU/GPU backend is refused. Initial route selection and replica
selection must satisfy the same policy; a faster GPU advert cannot override an
accepted CPU-only plan. Engine logs record the request and adapter-reported mask.
These flags are an adapter contract, not a kernel profiler or GPU speed measurement.

Low-level Segment callers retain `auto` when the variable is unset. Unsupported
values are errors, not automatic fallback. The ordinary household TUI does not
expose this internal policy as another user decision.

All pinned upstream Edge/Segment adapters currently support CPU execution.
Colibri remains unchanged. A future GPU backend needs a matching memory contract
(VRAM, scratch, state, RAM staging), explicit placement and an actual execution
test before household admission may request it. This guard does not implement
or advertise CUDA, Metal, HIP or Vulkan household execution.

Regression coverage includes strict policy parsing, rejected/mixed backend
masks, early rejection by both executables, and real two-donor generation with
an inherited GPU preference. The latter checks the CPU policy in both Segment
logs and in the actual Edge child's log, including native CI and installed paths.
