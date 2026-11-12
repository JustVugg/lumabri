# Resident GGUF checkpoints

In the TUI, open **Settings → Model folder or GGUF file** and select a folder
containing GGUF files, or a GGUF file directly. A split checkpoint uses its
first `-00001-of-000NN.gguf` file; keep every part together in the same folder.
The catalog shows one model for the complete set.

Select the computers, inspect the resident plan and request it. Each donor
must approve. Chat becomes available only after the assigned weights and
session resources have been prepared. As with other resident checkpoints,
closing chat leaves the approved weights loaded until their owner releases
them.

## Current execution contract

The integrated GGUF CPU paths cover OLMoE, Qwen3 dense/MoE, Qwen3.5/3.6
dense/MoE, Flint, Muse Spark, LFM3 text, GLM-5.2/5.3 (`glm-dsa`),
GLM5-Next text (`glm5-next`), Kimi K3 text (`kimi-k3`), Inkling text,
Qwen3.8 text (`qwen4exp`), DeepSeek V4 text (`deepseek4`) and Mistral Large 4 text. See
[Qwen architecture contracts](QWEN_ARCHITECTURES.md) and [Mistral](MISTRAL.md).
The inspector checks the
actual tensor names, dimensions, native quantization block geometry and
effective memory representation. GGUF support in Colibri alone does not grant
a distributed memory contract to another architecture. Unsupported or invalid
imports remain visible with an import error and cannot start a plan.

GGUF runs through Segment. The merged-int8 Hybrid expert provider is not
used for GGUF. Context uses the checkpoint's declared limit capped by the
adapter's limit (4096 for OLMoE, 262144 for Qwen, 1048576 for Mistral), with F32 KV state per session. There is no
disk-streaming fallback.

GLM's shared indexer selections cross Segment boundaries with the hidden
activations; per-conversation KV and indexer history stay on their owners.
The GLM inspector accepts native, split and `expert-packed-v1` containers,
validates each attention/expert tensor and prices native block layouts.
Optional NextN tensors are excluded from the text chain; this path does not
enable speculative decoding.

GLM5-Next uses its own `glm53` adapter. Its KDA recurrence and convolution,
pooled sparse-attention history and hyper-connections are accounted separately
from `glm-dsa`. Native, split and packed expert containers are validated before
admission; mixed native block types and tied heads retain their runtime layout.
The CPU contract covers NoPE text attention with an indexer on each MLA layer,
and a single SwiGLU clamp shared by those layers, as implemented by the pinned
engine. Vision and NextN generation are not enabled by this text contract.
`make test-glm53-gguf ENGINE="$COLIBRI_SOURCE" GLM53_GGUF_TEST_OPTIONS=--household`
checks monolithic/split tokens, independent conversations, snapshots, multiple
resident engines and the normal donor approval and chat flow.

Kimi K3 keeps attention-residual boundaries, KDA recurrence/convolution and
MLA history on the assigned segments. Its GGUF contract accepts native, split
and packed containers with block-quantized routed experts. Float dense weights
follow the engine's dense quantization settings; embeddings and residual-score
weights are prepared in engine-owned RAM before chat. The inspector accounts
for padded expert slots and a kernel-workspace reserve derived from the model
geometry, without the standalone streaming loader's page-cache reserve.
Floating routed-expert matrices, tied heads and DSA
extensions absent from this GGUF layout are rejected before admission.
`make test-kimi-gguf ENGINE="$COLIBRI_SOURCE" KIMI_GGUF_TEST_OPTIONS=--household`
checks every layer cut, session isolation, snapshot continuation and household
chat with the source offline. These reduced-checkpoint checks do not establish
performance for a full-size Kimi checkpoint.

Inkling's text path validates the distinct local/global KV heads, sliding
window, relative-position bank, convolution history, shared-expert routing
and unpadded output vocabulary. Native, split and `expert-packed-v1` GGUFs
keep the engine's block layouts, with resident space for its largest expert
slots, scales and row-interleaving overhead. Each conversation owns its KV
and convolution state. Embeddings and output weights are loaded before chat;
their quantized buffers are released when the allocation closes.
`make test-inkling-gguf ENGINE="$COLIBRI_SOURCE" INKLING_GGUF_TEST_OPTIONS=--household`
checks every cut against monolithic execution, snapshots across the sliding
window, interleaved conversations and models, and the normal approval/chat
flow with the source offline. This contract requires at least one shared
expert and does not enable audio or dense-quantization sidecars.

Qwen3.8 accepts native and split `qwen4exp` GGUFs with mixed native matrix
types. The inspector validates hyper-connections, QSA/indexer geometry,
DeltaNet state and the PLE hash ranges. The segment that owns the PLE layer
loads its complete table into engine-owned RAM; the same ownership applies
to Safetensors PLE tables. Metadata and GGUF arithmetic flags belong to each
model, so opening a second model cannot change the first model's computation.
This text contract uses one PLE layer and one indexer KV head, and does not
enable NextN, vision projectors or packed-expert GGUF containers.
`make test-qwen38-gguf ENGINE="$COLIBRI_SOURCE" QWEN38_GGUF_TEST_OPTIONS=--household`
checks all layer cuts against the pinned monolithic engine, mixed formats,
snapshot continuation, model unload and source-offline household chat.

DeepSeek V4 accepts native, split and `expert-packed-v1` text GGUFs. The
inspector checks hyper-connections, compressed attention, the indexer and
hash routing. Expert slots are budgeted at the largest record size across
the checkpoint, matching the pinned engine. Edge owns complete embedding
and output tables; no head or embedding row is fetched during inference.
The resident CPU contract requires block-quantized routed experts. Router
and index-head projections must be floating tensors because the current
upstream kernels consume BF16 values there. These constraints are checked
before admission. NextN generation is not enabled by this text contract.
`make test-v4-gguf ENGINE="$COLIBRI_SOURCE" V4_GGUF_TEST_OPTIONS=--household`
checks all cuts against unmodified monolithic execution, mixed block types,
compressed-state snapshots, model unload and source-offline household chat.

`make test-glm-gguf ENGINE="$COLIBRI_SOURCE" GLM_GGUF_TEST_OPTIONS=--household`
compares every layer cut against the pinned monolithic engine, including
shared sparse selections, mixed quantizations, conversation interleaving,
snapshot restoration and source-offline household chat.

The source computer generates small JSON metadata files using C. It preserves
the embedded tokenizer, pre-tokenizer, special tokens and chat template. There
is no Python dependency at runtime and no re-quantization. A hidden
`.lumabri-gguf-*` directory beside the source contains this metadata and hard
links to the original GGUF parts, so the weights consume no second payload
copy. The source folder must be writable and support hard links. Incomplete
imports are prepared privately before their directory is published.
These cache directories may be removed when no preparation is using them;
the original files are not removed with them. A hard link retains its weight
data if you delete the original name, until the cache link is also removed.

Workers use the usual authenticated, signed block transfer. They request
headers and the blocks containing their assigned tensors; they do not import
the original computer's absolute paths. Block granularity can include adjacent
unassigned bytes during preparation. Preparation buffers are released and
weight input is sealed before inference. The native Windows input adapter
keeps only headers in its sparse metadata files; POSIX workers retain no
weight payload in the disk mirror.

## Reproducible checks

With the pinned Colibri sources at `$COLIBRI_SOURCE`:

```sh
make household test-gguf-metadata test-gguf-catalogue test-gguf-runtime ENGINE="$COLIBRI_SOURCE"
python3 tests/integration/gguf_olmoe_fixture.py build/gguf-home --mixed --byte-vocab --split
python3 tests/integration/home_flow_test.py --models-dir build/gguf-home/weights-00001-of-00002.gguf --resident-default --expect-metrics
```

The reduced checkpoint tests compare 32 greedy tokens against the original
monolithic Colibri engine, through whole, split and fresh Segment sessions.
They cover single and split GGUF containers, Q8_0 blocks and mixed Q8_0/F16
expert layers. Linux also intercepts read calls during inference. The TUI test
requires a second conversation to work with the weight source stopped, checks
the approved ranges and memory observations, and rejects disk weight writes.
These are reduced-model integration checks, not measurements of large-model
speed or a claim that every Colibri backend has been validated.
