# Releasing Lumabri

The swarm is a mixed-version network by design: a chatter, a donor and the
tracker upgrade at different times, owned by different people. A release is
therefore a wire-compatibility statement first and a tarball second.

## The compatibility contract

- **Adding an op** is safe. An old tracker or node answers an unknown op
  with `LMB_ERR`; every new-op caller in the tree treats that as "feature
  absent" and degrades (doctor says "tracker too old", a NAT peer stays a
  coverage bit instead of a replica, targeted exec falls back to anonymous
  relay).
- **Changing an op's body** is not safe. Grow a versioned record instead:
  `LMB_SWARM_DETAIL_VERSION` and `LMB_EREG_STATS_VERSION` show the pattern —
  bump the version field, keep the old length constant around
  (`LMB_EREG_STATS_LENGTH_V1`), and gate parsing on what actually arrived.
- **The engine ABI** (`coli_*`) belongs to colibri. Lumabri builds against
  `ENGINE=../colibri/c`; the hook anchors are whitespace-tolerant and CI
  fails loudly (`make_patches.py --check-anchors`) when upstream truly moves
  code.

## Cutting a release

1. `main` green in CI — that already covers: warnings-as-errors, the unit
   and integration suite (survival, quarantine, NAT adoption, relay
   concurrency and failover, hedging, RTT refresh), sanitizers, the
   resident-hybrid gate and the Segment failover gate.
2. Run the soak on one machine: `python3 swarm_soak.py` against a local
   tracker with peer churn, at least 30 minutes.
3. Tag: `git tag -a vX.Y.Z -m "..." && git push origin vX.Y.Z`.
4. Build the binaries people actually deploy:
   `make all engines chatters ENGINE=../colibri/c` on the oldest glibc you
   intend to support, and note the colibri commit you built against in the
   tag message — the pair is the release.
5. Update the server(s) you operate **first** (they are the origin and the
   tracker: everyone else's experience depends on them), donors and
   chatters at their leisure. Mixed versions are supported; a peer too old
   to speak a feature simply doesn't get it.

## Version numbers

`MAJOR.MINOR.PATCH`. Bump MINOR for any new op or versioned-record bump;
PATCH for fixes with no wire change; MAJOR only if an existing op's body
changes shape (avoid: see above).
