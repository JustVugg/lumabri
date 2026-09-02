# Protocol and rolling-upgrade contract

Lumabri has one framed transport and several independently versioned payloads.
Changing one payload never permits silently reinterpreting another.

| surface | current marker/version | incompatible behavior |
|---|---:|---|
| frame envelope | `LMB1` | close before allocating an invalid frame |
| Expert manifest | `LEM2` | skip the executor; local/replica fallback |
| signed model identity | `MID1` | refuse to mix checkpoint roots |
| Segment request/state | `SEG2` / 2 | explicit unsupported/bad-request reply |
| Segment discovery | `SGD1` / 1 | publish no compatible route |
| Segment assignment | `GSA1` / 2 | release/retry assignment, never guess a range |
| Expert stats capability | `ECAP` / 1, `EST1` / 2 | fall back to legacy registration without stats |
| swarm execution/detail | `SWX1` / 1, detail 2 | omit unavailable telemetry, not compute identity |
| executor residency (`LMB_ERES` 70/71) | additive op | older node answers `ERR`; chatter treats residency as unknown, no penalty |
| encoded expert call (`LMB_EXEC2` 72/73, caps word in `ERES_R`) | additive op | a chatter sends `EXEC2` only to a node whose `ERES_R` carries `LMB_CAP_EXEC2`; the tunnel keeps `EXEC`; bf16 only for values that are exactly bf16 |

The model root, engine ID, source/build profile, numeric class, state schema,
dtype and width are compatibility data rather than decoration. A peer with a
different value is another execution class and cannot enter an existing
session. Transport encryption authenticates bytes in flight but does not make
two numeric classes equivalent.

## Rolling upgrade

1. Add decoders before emitters and keep old payloads accepted where the
   operation remains unambiguous.
2. Deploy trackers first, then origins/replicas, then chatters, then donors.
3. A Segment placement remains fenced by lease and route generation for the
   life of the session. Move it only through snapshot/restore/replay.
4. Mark a node draining before restart; wait for inflight work or migrate it.
5. Never change a marker's meaning. Allocate a new version and keep the failure
   explicit when backward decoding is impossible.
6. Run the mixed-version fixture before removing a legacy decoder.

The C structs may grow additively through `struct_size`; wire structs do not.
Runtime-only scheduler fields in route snapshots are intentionally excluded
from the discovery encoding, so a v1 tracker and newer chatter still agree on
the same signed placement fields.
