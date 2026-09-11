# Reusing household weights

An approved household plan keeps verified model data for the next request.
The default donor storage layout is:

```text
~/.lumabri/home/
  mirrors/<adapter-id>/cache/           reusable working mirror and block maps
  cas/<hash-prefix>/<chunk-hash>        reusable content chunks
  weights.lock                         one active owner of mutable mirrors
```

`--disk` places the same layout below its `lumabri-home` directory. A new
request still requires explicit approval. Edge and Segment processes are
restarted with fresh session state; these caches contain weights, not text
history or KV snapshots. Releasing a plan frees its RAM and both allocation
locks, but retains verified model files.

The adapter selects a working mirror, not a trusted identity. Protocol model
roots include the session routing name and therefore change between requests.
The existing loader still validates the accepted signed root, current
inventory and content hashes. A new identity resets the mirror's maps;
its blocks can then be rebuilt
from the local CAS without downloading matching weights again. Neither an
adapter name nor a cached filename can authorize old bytes for a new model.
Different homes
pointing at one cache directory cannot run competing mutable mirrors: the
second allocation fails promptly rather than waiting behind the first.

This is not zero network traffic. Discovery, authentication, manifests and
direct configuration reads still use the network. It is also not a disk
execution mode: the resident memory checks remain unchanged.

CAS chunks are a reconstructible cache. A complete chunk is written to a
private temporary file and atomically renamed, without forcing a file and
directory synchronization for each MiB in the inference thread. An abrupt
power loss can therefore lose recent CAS writes. On every use, size and hash
are checked against the accepted truth; missing or torn chunks require a
verified source, and fail with EIO if no valid copy is reachable. A bad cached
chunk is replaced rather than trusted because its hash-shaped filename exists.
This is not a durability guarantee for newly cached chunks. The mirror's
separate data-before-map synchronization and signed-truth checks are unchanged.

## Verification

`tests/integration/native_shim_test.py` observes native synchronization calls:
none target CAS during publication, while mirror synchronization still occurs.
It tests truncated and same-size corrupt chunks, nonblocking refusal/repair of
FIFO entries, symlink replacement without touching the target, and rejection
of corrupt CAS when the origin is offline. This simulates damaged cache state,
not a physical power-cut certification.

### Inspecting and clearing storage in the TUI

Open the workspace command list with `/` and select `/storage`. This view
counts allocated file blocks, not sparse files' apparent lengths, below the
current user's default `~/.lumabri/home/cas` and `mirrors` directories.
Hardlinked entries can be counted more than once, so the value is approximate.
It does not include source model folders, legacy caches, custom `--disk`
locations, logs or other applications' storage.

Clearing requires a second, explicit confirmation; the default is to keep
the weights. A plan holding the shared weight lease prevents clearing, even
if it starts after the overview was displayed. The lease inode is retained.
The operation validates both cache trees before deleting any entries, rejects
cross-filesystem trees and special files, and never follows symlinks. Leaf
links are removed without deleting their targets. Original checkpoint folders,
keys, settings, conversations and engine logs are not cleanup targets.

Scanning and clearing show progress and accept Escape between filesystem
operations. Cancellation during inspection deletes nothing; cancellation or
an I/O error during clearing can leave a partially cleared cache. Removed
weights are permanently deleted, not moved to Trash, and must be fetched
again. Clearing Linux files does not itself compact a WSL virtual disk.

This is manual cache management, not a maximum growth quota or automatic
eviction. The conservative admission reserve remains in force.

`test_weight_cache` covers lease exclusion, sparse accounting, symlinks and
hardlinks, unsafe-tree refusal, cancellation and lease release. The real PTY
test `tests/integration/storage_ui_test.py` checks the storage action, busy
refusal, the non-destructive default, confirmed cleanup and terminal restoration.

### Reuse between approved sessions

```sh
make household swarm_probe
python3 tests/integration/home_flow_test.py --models-dir /path/to/one-fixture-parent --repeat-cached --clear-cached
```

The test executes two separately approved plans on two real Segment
processes. It checks the same persistent cache paths and content chunks,
the source's published byte/read counters, generation in the second plan,
and release of RAM/cache locks. The repeated synthetic OLMoE request must
leave only the two direct `config.json` reads at the source. Native macOS CI
runs the same test; loopback is not a physical LAN certification.

With `--clear-cached`, the test then opens each donor's real workspace storage
view, explicitly clears its released weight cache and starts a third approved
plan. The source must serve weights again, both caches must be rebuilt and
generation and lease release must succeed. The same check runs against the
installed native candidate layout.

## Still outside this change

- Importing old per-request caches: existing directories are left untouched.
- Automatic eviction, a total disk-cache quota and cache-aware free-space admission.
- Remote checkpoint selection, authorized acquisition and model-license handling.
- A verified smaller disk working set, sessions sharing one donor/cache and KV replay.
  Independent plans on disjoint donors hold separate cache leases and can coexist.

The requester still supplies a local checkpoint. Gate 5 is not complete just
because verified weight blocks are reused.
