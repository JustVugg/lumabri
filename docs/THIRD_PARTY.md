# Third-party build inputs

## Colibri V4.1 boundary laboratory

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
  shared-state delta codec, session lifetime helper, experimental public-ABI
  wrapper and numerical/encrypted process-boundary oracle tests. DSpark and vision are excluded
  from this text boundary laboratory; they are not declared supported.
- The source checkout is never modified. The generated copy is not a general
  engine fork and is not linked into shipped Lumabri binaries.
