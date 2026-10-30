# Third-party build inputs

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

- Source: `JustVugg/colibri`, tag `v1.12.1`, commit
  `ce370e87d7b623d7759b52ec2007d75fc5b0e87e`.
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
