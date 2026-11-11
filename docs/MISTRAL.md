# Resident Mistral Large 4 text

Select the native GGUF, its first split part, or a Colibri `expert-packed-v1`
import in the model catalog. Lumabri reads the geometry, tokenizer and tensor
layout, then proposes resident CPU ranges using the GLM engine's Mistral path.
Each selected donor still approves its allocation before loading.

Mistral's layers share more than hidden activations. The boundary also carries
the current latent KV row needed by later layers and the sparse indexer's
selection. Older KV history stays with the executing segment and is separate
for each conversation. Splitting immediately before a shared-KV consumer or
shared indexer must produce the same continuation as the local engine.

The memory contract includes dense and expert weights, native block layouts,
learned RoPE data, KV/indexer state, temporary work and loading overhead.
Dimensions and control arrays must agree between the GGUF and generated config.
Malformed, incomplete or unsupported layouts fail admission. No disk-backed
inference fallback is enabled. The full context declared by a checkpoint is
not a promise that the selected computers have enough memory for it.

Text prompts use Mistral's `[MODEL_SETTINGS]`, `[INST]` and system markers.
Streamed `[THINK]` markers are normalized for the common chat UI and restored
when a reply becomes history. This path does not yet enable Mistral's vision
tower, native tool-call serialization, FP8 source conversion or GPU kernels.
Those capabilities are separate from native GGUF text execution.

## Validation

With the pinned Colibri checkout:

```sh
make household test-mistral-segments ENGINE=/path/to/colibri/c MISTRAL_TEST_OPTIONS=--household
```

The gate uses Colibri's eight-layer reduced Mistral fixture with shared KV,
shared sparse selections, learned RoPE, SSMax, and output softcapping. It checks:

- 32 greedy tokens against the unmodified monolithic engine, for all seven cuts;
- F32 and Q8 weights, native and packed experts, and split GGUF files;
- no weight reads during generation;
- interleaved conversations and restored continuations across every cut;
- malformed boundary state rejected without changing the live session;
- API prompt bytes against Colibri's renderer;
- catalog, approval, resident preparation and a second chat after source shutdown.

These checks validate the architecture on reduced checkpoints. They are not a
measurement of the full 1T model's speed or a validation of every platform and
quantization combination.
