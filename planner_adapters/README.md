# Adapter memory contracts

These are Lumabri-side descriptions, not changes to Colibri. They read small
checkpoint metadata before donor approval; they do not initialize an engine.
The implementation under test is pinned by CI to Colibri
`12a5c464b5c1f8292d578c62458706bc32d6ac95`.

## Qwen3.6 converted CPU checkpoints

`qwen36_meta.json` is authoritative for projection dimensions and layer kinds.
The contract accounts separately for:

- float32 dense matrices, norms and shared experts;
- all routed expert slots, including float32 group scales; packed int4 source
  weights expand to int8 in the CPU cache, so file size is not the RAM size;
- fixed DeltaNet recurrence/convolution state versus context-sized attention KV;
- float32 embedding, output head and final norm on Edge;
- per-engine structures and conservative bounded prefill/load workspace.

Scratch assumes at most 128 rows per engine request and 256 OpenMP threads,
the respective pinned adapter and Lumabri household limits. The process-level
reservation guard remains in force. Session state is multiplied by session
count; engine weights and workspace are shared under the serialized run gate.

Tests use synthetic converted int8 and grouped-int4 checkpoints. Linux checks
the int8 output against an independent PyTorch oracle; both encodings exercise
the complete household approval/execution path. Grouped int4 also exercises
split-token equivalence and lost-donor cleanup. macOS CI runs the int4 household
path on Intel/Apple Silicon with and without OpenMP. These are not large-model
performance results, and do not certify GPU or disk execution.

## Inkling CPU checkpoints

The planner reads bounded safetensors headers, not weight payloads. It reserves
the tensors that the native adapter actually loads for each interval and Edge:
BF16 dense matrices stay BF16; F16 dense matrices expand to float32; converted
int4/int8 experts stay packed, with float32 row scales. Float-source experts
reserve their float32 upper bound. Optional dense-quantization sidecars reserve
both banks conservatively because the native loader can fall back per tensor.

Per-layer KV uses either the full context or the configured sliding ring.
Convolution state is separate, and each session gets independent state. Scratch
includes bounded 128-row prefill, up to 256 threads, relative-attention buffers,
and a loading temporary. Missing tensors, duplicate used tensors, truncated
files, incoherent packed encodings, and invalid scale sizes reject sizing.
Disk execution is not enabled by this resident contract.

Linux checks 24 generated float tokens against an independent PyTorch oracle,
then runs approved two-donor household execution. Packed int4 additionally
checks whole/split equivalence and lost-donor cleanup. Native macOS Intel/ARM
CI runs the int4 household flow with and without OpenMP. The fixture generator
retags its synthetic `inkling_text` export as the pinned registry's `inkling`;
this is not a new runtime alias or a claim about arbitrary exports. The tests
do not certify large checkpoints or GPU performance.

Other families must supply their own retained-weight and state contracts.
Do not promote them by changing `sizing_verified` alone, or reuse ordinary GQA
KV arithmetic for recurrent/latent/hyper-connection architectures.
