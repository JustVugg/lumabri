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

Other families must supply their own retained-weight and state contracts.
Do not promote them by changing `sizing_verified` alone, or reuse ordinary GQA
KV arithmetic for recurrent/latent/hyper-connection architectures.
