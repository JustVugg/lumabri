# Third-party build inputs

## Colibri GLM GGUF integration

- Source: `JustVugg/colibri`, tag `v2.1.0`, commit
  `704a3f68184b78367a04cb5526e428dc2f90f05b`.
- Upstream files: `c/colibri.c`, `c/st_gguf.h`,
  `c/tools/gguf_import.py`, `c/tests/test_gguf_import.py`, and
  `c/tests/test_segment_adapters_real.c`.
- License: Apache-2.0; attribution: Colibri contributors. Original license,
  headers and notices remain with build copies and distributed runtimes.
- Lumabri supplies C metadata/geometry validation and memory accounting,
  extending its shared-state boundary to carry GLM's sparse selections.
  Engine arithmetic is unchanged. Tests invoke upstream mixed-quantization
  fixtures and monolithic execution, remove selected indexer banks to exercise
  sharing, and also dequantize a fixture to test the float-source path.
  A declared tied head loads its embedding tensor through the same native
  loader; its independent oracle uses an explicit head with identical bytes.

## Colibri GGUF declarations

- Source: `JustVugg/colibri`, tag `v2.1.0`, commit
  `704a3f68184b78367a04cb5526e428dc2f90f05b`.
- Source files: `c/ggml_quants.h` and `c/tools/gguf_import.py`.
- License: Apache-2.0; attribution: Colibri contributors. Preserve the
  upstream license and notices with generated sources and packaged runtimes.
- Derived files: `planner_adapters/gguf_types.h` contains container/block
  geometry; `src/planner/lumabri_gguf_tokenizers_data.h` contains tokenizer
  regex declarations and BPE flags, converted to C data tables. Neither
  includes execution kernels. Generation and drift checks live in
  `tools/sync_gguf_types.py` and `tools/sync_gguf_tokenizers.py`.
- Lumabri-owned integration: bounded header inventory, metadata serializer,
  resident memory contracts and generated-copy hooks for quantized Edge
  boundaries, header I/O and metadata lifetime. Upstream originals remain
  unchanged; original source headers stay in generated adapter copies.

## Colibri qwen36 architecture integration

- Source: `JustVugg/colibri`, tag `v2.1.0`, commit
  `704a3f68184b78367a04cb5526e428dc2f90f05b`.
- Upstream files: `c/qwen36.c`, `c/tests/test_segment_adapters_real.c`,
  `c/tools/make_qwen36_tiny.py`, `c/tools/make_flint_tiny.py`,
  `c/tools/make_lfm3_tiny.py`, `c/tools/convert_qwen36.py` and
  `c/openai_server.py` (checkpoint template detection and text rendering).
- License: Apache-2.0; attribution: Colibri contributors. Original copyright,
  source headers and license files remain in generated copies and packages.
- Lumabri changes: generated-copy hooks skip routed expert preparation in
  dense layers, probe the checkpoint's routed encoding independently of the
  interval boundary, release new layer-specific allocations, and allocate
  LFM3's per-session convolution placeholder portably. The planner inspects
  each layer and accounts for the runtime's CPU weight/state representation.
- Native GGUF hooks give each adapter its own source context, serialize the
  upstream global mode during callbacks, own/free native expert indexes, and
  use per-model int4 decisions. The Edge head budgets its actual block layout.
  GGUF inventory and memory inspection are Lumabri code; native numeric kernels
  remain in the pinned upstream source. Tests additionally invoke
  `c/tools/gguf_import.py` and `c/tools/gguf_reader.py` without modifying them.
- Tests invoke upstream fixture/conversion tools and the original monolithic
  engine from the pinned checkout; no model kernels are rewritten. LFM3 test
  tensors are renamed from the generator's GGUF names to its converter's HF
  names without changing their values. The upstream checkout is read-only.
- Flint escaping and LFM3/Coder prompt conventions are expressed in C in
  `src/runtime/lumabri_chat_format.h` and `lumabri_chat_checkpoint.h`;
  their API output is compared byte for byte with the upstream Python
  renderers, which are only loaded in tests, without starting their server.

- Muse Spark extension: the same pinned revision's `c/tools/make_muse_tiny.py`
  supplies fixture weights and `c/openai_server.py` specifies Muse prompt and
  channel framing. `src/runtime/lumabri_muse_chat.h` is a C adaptation with
  bounded streaming state and shared TUI/API formatting; its template literals
  follow that Apache-2.0 source (Colibri contributors). Lumabri supplies the
  latent-width memory contract and boundary/state/inference-read tests.

## Colibri Mistral Large 4 integration

- Source: `JustVugg/colibri`, tag `v2.1.0`, commit
  `704a3f68184b78367a04cb5526e428dc2f90f05b`.
- Upstream files: `c/colibri.c`, `c/telemetry.h`, `c/st_gguf.h`, `c/openai_server.py`,
  `c/tools/make_mistral_large4_tiny.py`, `c/tools/gguf_import.py`, and
  `c/tests/test_segment_adapters_real.c`.
- License: Apache-2.0; attribution: Colibri contributors. Original headers,
  license and notices remain in generated sources and packages.
- Lumabri-owned generated-copy hooks carry cross-layer latent rows and sparse
  selections with activations, keep conversation KV on each owning segment,
  serialize source metadata opening, and free model-specific allocations.
  Model arithmetic remains in the pinned engine. The RoPE cache key also
  includes the learned frequency table to isolate different models. The
  standalone dashboard hit bitmap is disabled in embedded adapters; Lumabri
  reports per-allocation metrics without sharing first-model-sized telemetry.
- `src/runtime/lumabri_mistral_chat.h` and the shared chat formatter express
  the upstream text framing in C with bounded stream state. Tests compare
  the API prompt against the original Python renderer. Fixture generation,
  quantization and monolithic numerical oracles use the upstream files;
  none of these tests changes the original checkout.

## Embedded image decoder

- File: `src/vendor/stb_image.h`, unmodified v2.30 from commit
  `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` of `nothings/stb`.
- SHA-256: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.
- Copyright: Sean Barrett and contributors. The complete dual MIT/public-domain
  license remains in the header. Lumabri uses the MIT option.
- Integration: PNG/JPEG memory decoding only, no file I/O, bounded dimensions
  and allocation accounting. Resizing and model patch preparation are separate
  Lumabri code in `src/runtime/lumabri_image.c`.

## Colibri V4.1 resident adapter

- Source: `JustVugg/colibri`, tag `v2.1.0`, commit
  `704a3f68184b78367a04cb5526e428dc2f90f05b`.
- Upstream files: `c/deepseek_v41.c`, its C headers, and
  `c/tools/make_dsv41_tiny.py` / `c/tools/dsv41_ref.py`.
- License: Apache-2.0, supplied by the upstream repository's `LICENSE`.
  Preserve upstream license/copyright/notice files when distributing generated
  sources or binaries. This gate does not bundle upstream source or models.
- Attribution: Colibri contributors. The numerical kernels, fixture and
  reference math remain upstream-owned.
- Lumabri changes: exact, hash-pinned build-copy hooks for partial range loading,
  residual/mHC boundaries and resident packed Engram tables; a Lumabri-owned
  shared-state delta codec, session lifetime helper, public-ABI wrapper,
  checkpoint memory inspector and numerical/encrypted process-boundary oracle
  tests. DSpark and vision are excluded from this text adapter.
- The source checkout is never modified. The generated copy is not a general
  engine fork. It is linked through the Lumabri-owned adapter into household
  binaries; Colibri's upstream adapter registry remains unchanged.
