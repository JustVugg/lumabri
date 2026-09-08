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

## Verification

```sh
make household swarm_probe
python3 tests/integration/home_flow_test.py --models-dir /path/to/one-fixture-parent --repeat-cached
```

The test executes two separately approved plans on two real Segment
processes. It checks the same persistent cache paths and content chunks,
the source's published byte/read counters, generation in the second plan,
and release of RAM/cache locks. The repeated synthetic OLMoE request must
leave only the two direct `config.json` reads at the source. Native macOS CI
runs the same test; loopback is not a physical LAN certification.

## Still outside this change

- Importing old per-request caches: existing directories are left untouched.
- Automatic eviction, a total disk-cache quota and cache-aware free-space admission.
- Remote checkpoint selection, authorized acquisition and model-license handling.
- A verified smaller disk working set, concurrent sessions and KV replay.

The requester still supplies a local checkpoint. Gate 5 is not complete just
because verified weight blocks are reused.
