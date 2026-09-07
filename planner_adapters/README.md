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

Fixture correction: the first version of the int4 test passed HF-named tensors
straight to a converter that quantizes TML-named tensors only. That output was
still float; those earlier runs are not packed-format evidence. The generator
now uses the upstream reverse-mapping helper and checks U8 payload and F32 scale
byte counts for every routed tensor before any engine test can run.

## Kimi CPU checkpoints with float dense source and MXFP4 experts

The initial contract validates the complete MXFP4 expert tensor bank (packed
nibbles and byte exponent scales), including native per-slot alignment padding.
Dense tensors reserve a conservative float32 upper bound plus scales, covering
the native load-time quantization profiles; prepared U8 dense containers remain
unverified rather than being mistaken for float tensors. Embedding row reads
are conservatively covered by a whole-boundary reservation, not disk-mode admission.

KDA recurrence and convolution state are fixed per session. MLA latent/rotary
and optional DSA index caches grow with context. Edge/Segment exchange AttnRes
state with width `hidden * (1 + ceil(layers / block_size))`, not just `hidden`;
both boundary buffers and loading workspace are reserved. Expert capacity is
an upper bound: native `K3_EXPERT_GB`/RAM policy can choose a smaller cache, so
this contract does not certify that every admitted weight was warmed into RAM.

Admission also reserves the pinned initializer's unconditional 3.7 GB policy
floor, plus its whole-model KV projection. The policy is not measured tiny-model
RSS, but ignoring it produces plans the native engine refuses to open. We do not
enable `COLI_RAM_OVERCOMMIT` to bypass it. The standard macOS ARM CI runner tests
insufficient-memory refusal; two-range Kimi execution needs a larger machine.

The numpy-generated tiny fixture retains native MXFP4 experts. Its eight-token
independent oracle comes from upstream's hash-pinned Moonshot reference, in the
explicit exact numeric profile. Linux also checks whole/split equivalence and
the household flow, including cleanup under the default numeric profile.
macOS Intel CI runs household execution with and without OpenMP. Native ARM
Kimi execution remains unverified on a sufficient-memory machine.

## GLM CPU float-source checkpoints

The initial contract accepts separate float/BF16/F16 expert matrices and dense
weights. It reserves float32 plus a conservative scale/padding bound, latent MLA
and rotary state, optional per-layer DSA state, and both loader temporaries and
the native `ws[64]` miss slabs. Edge load quantization gets a separate temporary.
Prepared `.qs` containers are not silently treated as ordinary tensors: their
format stamps and mixed quantization layouts still need dedicated validation.
This initial bound requires all matrix contraction dimensions to be at least
32; smaller synthetic geometries need separate packed-row padding accounting.

An incomplete DSA bank is rejected: Edge detects indexer support globally,
whereas each Segment detects it for its interval. Admitting only part of that
bank would give incompatible numerical classes after splitting.

The generated float fixture matches 20 independent PyTorch oracle tokens with
native `GLM_SEGMENT_EBITS=16 GLM_SEGMENT_DBITS=16`. Linux checks whole/split
equivalence and approvals; default CPU quantization also exercises lost-donor
cleanup. Native macOS Intel/ARM runs household execution in both OpenMP modes.

## GLM5.3 text-only float-source checkpoints

This contract counts hyper-connection boundary width, separate KDA and latent
DSA state, conservative float/quantization workspace, and context-sized prefill.
The pinned adapter allocates state for every model layer in every Segment
session, even for a one-layer interval. That allocation is represented as
engine-wide session state, not divided by the number of owned layers.

The stateless Edge advertises a zero context limit (caller-selected). Lumabri
uses its protocol ceiling in that case and still negotiates the actual Segment
limits. It no longer rejects every GLM5.3 household chat at argument parsing.

The initial contract rejects packed containers and vision-config checkpoints:
the native loader replicates the vision tower on each process, even for text
chat. It also refuses the pinned loader's incomplete pool=1 path. These need
their own validated layouts; they are not promoted by the text-only oracle.
The tiny fixture has four independent Transformers continuation tokens.
Whole/split and household execution are separate tests in Linux and native
macOS CI; they do not prove full-size memory or performance.

Other families must supply their own retained-weight and state contracts.
Do not promote them by changing `sizing_verified` alone, or reuse ordinary GQA
KV arithmetic for recurrent/latent/hyper-connection architectures.
