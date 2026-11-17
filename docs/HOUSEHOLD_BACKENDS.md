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

All pinned upstream Edge/Segment adapters support CPU execution, and the CPU
policy above stays the only memory contract: every assigned weight is resident
in RAM and the reservation a donor approves is a RAM reservation.

## CUDA acceleration of a Qwen3.6 range

A runtime built with `make CUDA=1` (Linux, NVIDIA toolkit at `CUDA_HOME`,
default `/usr/local/cuda`) links the engine's own CUDA expert tier into
`segment_node`. A donor that runs such a runtime computes its Qwen3.6 range on
the card:

- the weights are loaded into RAM exactly as planned and approved, and the
  routed experts and the dense trunk of the assigned layers are also copied to
  the card, as far as its free memory allows; anything that does not fit is
  computed on the CPU from the RAM copy;
- the plan, the reservation and the approval screen do not change, and VRAM is
  not planned or reserved: it is used when free and not required;
- the chat host's Edge stays on the CPU;
- the Segment advertises `CPU | CUDA` and logs
  `backend policy=cpu adapter_mask=0x300 (resident in RAM, computing on CUDA)`.
  The CPU policy accepts exactly that combination, so a household can mix
  donors with and without a card. Any other backend is still refused.

`LUMABRI_GPU=off` in the donor's environment leaves the card untouched. A
default build contains no CUDA code and needs no toolkit; `segment_node
--backends` prints what a runtime was built with. Colibri remains unchanged:
`tools/prepare_cuda_segment.py` edits Lumabri's build copy and fails the build
if the pinned engine source no longer matches.

Measured on one computer (14 cores, one 24 GB card), real Qwen3.6 35B GGUF
(Q4_K_M), whole model on that computer: 82 ms per token on the CPU, 34 ms with
the card. The same model split between that computer (layers 11 to 40, on the
card) and a second, CPU-only computer 55 to 70 ms away (layers 0 to 11, chat
host): 192 ms per token, against 280 ms for a comparable split with both on
the CPU. Not measured: other cards, several cards, Windows.

Every computer of the household needs a runtime with this policy: an older
chat host refuses a Segment that advertises `CPU | CUDA` and reports that no
chain can be formed.

Limits: Qwen3.6 only (`qwen36` engine; OLMoE, Qwen3.8 and the other families
stay on the CPU); the catalogue's measured speed does not yet distinguish a
donor that switched its card on or off; Metal, HIP and Vulkan household
execution are not implemented or advertised.

Regression coverage includes strict policy parsing, rejected/mixed backend
masks, early rejection by both executables, and real two-donor generation with
an inherited GPU preference. The latter checks the CPU policy in both Segment
logs and in the actual Edge child's log, including native CI and installed paths.
