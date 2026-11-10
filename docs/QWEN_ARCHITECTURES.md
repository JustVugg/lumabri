# Resident Qwen architecture support

The `qwen36` adapter handles several text architectures. Lumabri inspects the
checkpoint's configuration and tensor headers before
allowing a resident plan. Selecting a model follows the normal catalogue,
donor approval and preparation flow.

| Architecture | Converted Safetensors and native GGUF CPU paths |
| --- | --- |
| Qwen3 dense | Full attention and dense MLP |
| Qwen3 MoE / Qwen3-Coder | Full attention, routed experts, no shared expert |
| Qwen3.5 dense | Attention, DeltaNet and dense MLP |
| Qwen3.5 / 3.6 MoE | Attention, DeltaNet, routed and shared experts |
| Flint | Dense and MoE layers, sliding attention, learned sinks and per-layer RoPE |
| LFM3 text | Attention and short convolution with separate state per session |

Use containers produced by the pinned Colibri converter. For routed experts,
the inspector supports int8, packed int4, and mixed int4 gate/up with int8 down
projections, including their different scale groups. Dense CPU matrices use
F32 in the distributed adapter; the memory plan prices that representation.
For native GGUF, quantized matrices retain their block format, including any
CPU row packing; floating matrices and the Edge embedding expand to F32.
Reservations include header indexes, conversion/repacking scratch and the
largest expert slot used by the runtime. Native GGUF experts do not use the
old int8-container size estimate. Single and split files, tied output heads
and Colibri's `expert-packed-v1` layout are supported.

The decoder keeps F32 KV and recurrent state. Its maximum context is the
checkpoint's declared limit capped by the adapter's limit.

The shape of each layer determines its reservation. A dense layer does not
reserve or load nonexistent routed experts. DeltaNet and LFM3 convolution
state remain separate for each conversation. Weights load before READY;
generation does not fall back to checkpoint reads.

Chat format is checkpoint-specific too. Flint uses its role markers; LFM3
uses its BOS and history rules; Qwen3-Coder is identified by the checkpoint's
template and uses ChatML without thinking markers. Edge announces this
format before READY and Hosted passes it to TUI and API clients. A shared
compute adapter does not force every checkpoint to use the same prompt.

This contract covers text inference. It does not enable vision,
audio or GPU execution for these architectures. The catalogue exposes only
operations with an implemented execution contract.

## Validation

```sh
make test-qwen-architectures ENGINE=/path/to/colibri/c
make test-qwen-gguf ENGINE=/path/to/colibri/c QWEN_GGUF_TEST_OPTIONS=--packed
```

The test generates small checkpoints locally, uses Colibri's converter, and
compares the original monolithic engine with one range, two ranges and fresh
sessions. It also checks chaining, independent state, snapshots and rejected
corrupt restores. On Linux the numerical oracle forbids reads during
inference. These checks establish runtime correctness, not large-model speed.
`QWEN_TEST_OPTIONS=--household` also runs donor approval and resident chat
on loopback, then starts another private conversation with the weight source
offline. Linux CI runs this gate for dense Qwen, Qwen3-Coder, Flint and LFM3.

The native GGUF gate compares the same file through the unmodified upstream
engine and resident Edge/Segment adapters. It checks F32/Q8_0 payloads, split
files, packed experts, malformed shapes and required tensors. Add
`QWEN_GGUF_TEST_OPTIONS="--household --packed"` to exercise raw-file catalogue
import, approvals and source-offline second conversations for each family.
