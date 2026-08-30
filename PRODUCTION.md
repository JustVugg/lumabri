# Production release gate

The release gate separates correctness from the commercial speed claim. A
green local CI run says the protocols, security boundaries and six adapter
hooks still work. Only a distinct-host benchmark can say that more resident
peers increased tokens per second.

## Preflight a machine

Build or install Lumabri, then run:

```sh
lumabri doctor --tracker 148.251.4.122:7300 --model /srv/models/MODEL --serve-port 7300
lumabri doctor --json --tracker 148.251.4.122:7300 > doctor.json
```

`doctor` exits non-zero for a missing required binary/library, unreadable
model, unwritable state directory, unreachable requested tracker or occupied
serve topology. Low RAM, swap, disk and optional Segment binaries are warnings:
chat/Expert/CAS can remain correct while donation is paused or Segment is not
installed. The JSON schema is versioned and suitable for deployment checks.

## Local release evidence

```sh
./production_gate.sh --engine ../colibri/c --output artifacts/rc-local
```

This runs exact patch anchors, Werror, the full integration suite,
ASan/UBSan, and Segment Hybrid when the selected Colibri checkout exposes the
additive ABI. Every step has a separate log and the final decision is written
to `production-gate.json`. Colibri is copied only below `build/`; the selected
checkout is never modified. Fixture generation requires the versioned Python
packages in `requirements-test.txt`.

ThreadSanitizer is available separately because some container runtimes cannot
map its shadow memory:

```sh
make test-thread-sanitize
```

## Required multi-host release evidence

Prepare the explicit SSH/command spec described in [SWARM_BENCH.md](SWARM_BENCH.md),
then run:

```sh
./production_gate.sh --engine ../colibri/c --output artifacts/rc-multihost \
  --multi-host run.json --require-multihost
```

The gate records A (single-chat tok/s), B (aggregate tok/s at 1/2/4/8 clients)
and C (Expert calls per peer), rejects any token-oracle mismatch, requires at
least three measured peers and requires A to be at least 2x the faster of the
local-Colibri and single-Lumabri baselines. It does not configure SSH, install
keys, start somebody else's computer or infer that a connected-but-idle donor
helped.

For a long stability run, add seconds explicitly (259200 is 72 hours):

```sh
./production_gate.sh --engine ../colibri/c --output artifacts/rc-soak \
  --multi-host run.json --require-multihost --soak-seconds 259200
```

The soak repeats the exact oracle request, writes its result atomically and
keeps every failure with the elapsed timestamp. A process crash, malformed
JSON, token divergence or zero successful runs fails the release.

## Release checklist

1. Every stacked PR is reviewed and merged in order; no branch is skipped.
2. `lumabri doctor --json` is green on origin, chatter and each donor.
3. The local production gate artifact is attached to the release.
4. The multi-host decision is green; report the measured speedup, never a
   projected one.
5. Run fault injection: dead Expert, dead Segment, relay-only donor, pressure
   pause/recovery and occupied startup ports.
6. Verify direct firewall ports and then verify relay-only operation with those
   ports closed.
7. Run the chosen soak period and retain its JSON.
8. Confirm model licences allow redistribution before public CAS seeding.
9. Publish the exact Lumabri commit, Colibri commit, compiler, ISA and numeric
   profile used by the evidence.

Rolling upgrades must follow [PROTOCOL_COMPATIBILITY.md](PROTOCOL_COMPATIBILITY.md);
in particular, drain/migrate Segment sessions instead of changing a fenced
placement underneath an active conversation.

## Honest limits

- The executor sees activations; transport encryption is not confidential
  computing.
- Current Colibri Segment engines serialize one opaque engine instance. The
  range pipeline and replicas provide safe multi-chat parallelism; unrelated
  sessions are not fused and labelled continuous batching.
- A READY peer can be ignored when its predicted completion is worse than the
  fallback. More connected peers is not itself proof of more speed.
- Public operation still needs an abuse policy, operator monitoring and model
  licence review even when all technical gates pass.
