# Build inputs are not model caches

The household build used to copy the entire configured Colibri `c` directory
before patching its generated build copy. A local checkout can also contain
large converted checkpoints, tiny fixtures, virtual environments and previous
builds. Copying all of those into every Lumabri worktree was unnecessary and
could consume substantial disk space before compilation even started.

`tools/prepare_engine_source.py` now copies only recognized source/build files
and source notices. It works with a source archive without Git metadata. Model
weights, tokenizer/config JSON, binaries, object archives and common generated
directories are excluded. No checkpoint payload is read or downloaded by this
preparation step. Test fixtures belong in explicit test directories, not in the
generated engine source copy.

Preparation is bounded to 256 MiB of source inputs and 100,000 directory entries.
The current pinned archive is well below these limits. Missing required headers,
source symlinks/special files, overlapping paths and unsafe destinations fail
with an explanation rather than producing a partial engine tree.

A new copy is prepared in a sibling temporary directory before replacing the
previous generated copy. Copying failure preserves the old tree; publication
failure attempts to restore it. Replacement requires a build ownership marker.
For upgrades, only the exact historical `build/segment-hybrid-colibri` directory
with its `.prepared` marker and runtime headers is recognized as an older owned
copy. Its redundant files are removed on replacement, not the original Colibri
files or checkpoints. An arbitrary destination is never recursively erased.

Colibri itself stays unchanged. The existing Lumabri hooks are applied only to
the generated build copy, as before. This is build-storage hygiene, not automatic
eviction or a quota for runtime model caches.

## Incremental builds track every copied input

`build/segment-sources` records a SHA-256 fingerprint of the same bounded source
set used by preparation. Names, contents and copied permission bits participate;
adding, removing or changing an adapter, header or build script invalidates the
generated tree and therefore its archive and linked Segment binaries. This
replaces the historical dependency list of six model C files, which omitted
GLM5.3, Qwen3.8 and headers. Edits with a preserved mtime are still detected.

An unchanged fingerprint preserves the stamp's mtime and does not force another
copy or compilation. Build options still have their separate stamp. Weights,
tokenizer JSON and generated outputs neither enter this hash nor trigger a
rebuild. Computing it reads only the source inputs (about 9 MB at the pin),
never a model checkpoint. Invalid source inputs fail the build before the old
stamp or prepared tree is replaced. This does not make the upstream checkout
transactional: do not edit it concurrently with compilation.
