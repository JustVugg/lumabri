<img src="logo.svg" alt="lumabri: tiny engine, immense swarm" width="524">

Run huge mixture-of-experts models across the computers you already have,
with the [colibri](https://github.com/JustVugg/colibri) engine. Pure C, no
dependencies.

The model is split into contiguous ranges of layers, one range per machine,
and each node keeps the state for its own layers. Activations cross the
network between them; the weights stop moving once they are in place. Nothing
is downloaded up front: the bytes an inference actually touches arrive from a
peer on first use and stay in the shared CAS. Colibri's ordinary executables
and local-inference behaviour are unchanged.

**Adding computers always adds memory, capacity and availability. It makes a
single chat faster only when the new plan removes disk reads or brings faster
hardware** — layers are sequential, so twelve on one machine cost about what
six plus six cost on two, plus a hop. The case that changes everything is the
other one: a model that one machine would stream from NVMe and three hold in
RAM. See [README_HOME.md](README_HOME.md) for the catalogue, the three states
and the hosted chat.

Any machine may join, GPU or not. The engine was built for CPU and SSD first; a
GPU only makes it faster, never different, and the output is byte-for-byte the
same either way. Networks that pool GPUs recruit from the few. lumabri recruits
from everyone.

## Quick start

```sh
make ENGINE=/path/to/colibri/c
```

On the machine that has a model (any colibri model directory):

```sh
./lumabri serve --model /path/to/model
```

On a machine that wants to chat:

```sh
./lumabri chat --tracker <server-ip>:7300
```

That is all. On a swarm-fed model the dense weights (everything but the
routed experts) are pulled into the mirror before the chat says ready, with
a bar, the rate and an ETA; the first answer then costs only the network
round trips, not a layer-by-layer download. If no complete compatible Segment route exists, the same command
automatically falls back to the existing expert/CAS engine, then to local
execution as coverage permits; there is no separate `segment_chat` command for
an end user.

No model at hand? `make fixture` builds a tiny synthetic one so every step above
is real, just small.

### Just the terminal UI

```sh
lumabri
```

No arguments. It asks for the swarm address and, once, for the operator public
key, finds the engines itself, and remembers all of it in `~/.lumabri/config`.
The second time it is Enter, Enter, and you are in. Flags still win when you give
them, so a script never inherits somebody's saved answers.

When you join it also asks how you want to take part — just chat, or **lend your
machine too**. `disk` receives complete rarest-first files up to the chosen GB
budget (transferred and verified in MiB blocks). `compute` holds resident experts assigned by the tracker; it takes a Segment slice only when no expert executor exists for the model. It publishes
direct P2P when reachable and otherwise uses the signed outbound Segment relay;
the finer-grained expert donor remains the lower-memory fallback.
Both run at low priority and die with the TUI. Nothing to configure — Enter
picks what this machine can afford: with at least 2 GB of RAM free beyond
the system reserve it chats **and** donates compute, otherwise it just
chats. The menu says which before you press it; `1` always means chat only.

`lumabri machine` shows the quick startup profile used for donation: CPU
model/ISA, physical and logical cores, NUMA, RAM/swap, visible GPU/VRAM, disk,
interfaces and optional tracker RTT. `--json` emits a stable schema for
automation. `lumabri limits` shows the current donation budget; `lumabri pause`
and `lumabri resume` control every local donor without killing the chat.
On kernels without `/proc/meminfo`'s `MemAvailable` (notably WSL1), the same
conservative reclaimable-cache fallback is shared by doctor, Segment, Expert
and the governor; modern Linux continues to use its native value unchanged.
`lumabri doctor --json` is the versioned deployment preflight for required
binaries, CAS state, model readability, tracker reachability and the complete
serving port block. The reproducible local, sanitizer, multi-host and soak
release gates are documented in **[PRODUCTION.md](PRODUCTION.md)**.

While you donate, the frame above the idle prompt shows what your machine is
doing for the swarm and refreshes every few seconds: experts held (and how
many are resident in RAM), calls served and their rate, work in flight, and
bytes your storage served. A line starting with `/` that is not a known
command is refused with a hint instead of being sent to the model.
Inside the chat, `/swarm` (or `/hosts`) shows stable human-readable machine
names, storage served, expert calls and live Segment ranges. `/experts` answers
how often each executor has actually been used; `/model`, `/debug`, `/storage`
and `/help` expose the other controls. Tab completes slash commands. The prompt
is a real line editor — arrow keys, Home/End, word and line kill, and Up/Down
through the session's history — so a typo is one keystroke back, not a retyped
line.

Generation streams as soon as decoded text is stable. During inference a small
dock remains fixed above the input and reports routing, prefill, decode,
checkpoint or failover without splicing diagnostics into the answer, plus a
live count of what the swarm did for this reply: expert calls, the share
that went to donors, the busiest executor, and the bytes storage donors
served (read from the tracker every two seconds, off the token stream). The input
remains active: read-only menus open immediately and one next prompt or command
can be prepared while the current KV transition finishes. `Ctrl-C` interrupts
the active turn and exits immediately instead of waiting for it to finish;
`Ctrl-Z` restores the terminal before suspending and rebuilds the dock after
resume, while `Ctrl-\\` follows the same immediate shutdown path through
`SIGQUIT`. Normal exit, `SIGINT`, `SIGTERM`, `SIGHUP` and `SIGQUIT` all restore
the original terminal mode. If a child is genuinely uninterruptible, a second
Ctrl-C forces the chatter out after the first has already restored the shell.
As on every Unix program, `SIGKILL` cannot run cleanup; `reset` remains the
recovery command after an external `kill -9`.

## How it works

**Sharing bytes.** `serve` runs two small programs: a tracker, which is only an
index of who holds which files, and a maintainer, which answers byte-range reads
on the model directory. A maintainer can hold a slice of a model, and several
maintainers can share one.

**Reading bytes.** `chat` mounts the model through `liblumabri.so`, an
`LD_PRELOAD` shim that interposes the handful of libc calls an engine makes on a
model directory (`open`, `fopen`, `opendir`, `pread`). Files appear as sparse
local mirrors of the true size, so `fstat`, `readdir` and the page cache work
natively. A missing block is fetched from a peer, written to the mirror, and
then the engine's own `pread` proceeds. A warm read is a table lookup plus a
normal local read: no FUSE, no daemon on the read path. Every verified MiB is
also stored by sha256 in a local content-addressed store. The default CLI path,
`~/.lumabri/cas`, is shared by every checkpoint, so equal chunks are downloaded
once and can rebuild a different sparse mirror without a byte server.

**One rule, inherited from colibri:** the network may change where bytes come
from, never which bytes. Writing a model file returns `EROFS`. A block no peer
can serve is a loud `EIO`, never silent zeros. Byte identity is verified cold,
warm, and with every peer dead.

**Layers run as stateful segments.** `serve` reads `model_type`, layer count,
context, CPU count, available RAM and reachable network address, then starts a
small set of disjoint layer-aligned executors automatically. The origin ranges
are marked fallback. Compute donors ask only for a model; the tracker promises
the least-replicated exact range while they load it, and the asynchronous route
selector prefers their ordinary ranges over the matching origin ranges on the
next turn. Thus the original server begins with complete coverage and loses its
pipeline progressively without ever making the route incomplete. Every turn
uses one request per segment, with its model-specific KV/recurrent/conv state
kept in an isolated remote session.

The local Edge opens through `liblumabri.so`, so a user supplies neither a model
directory nor roots: the signed aggregate model identity comes from the tracker
and only blocks actually read by tokenizer/embedding/head enter the CAS. Four
origin ranges is the current latency/replacement compromise. Segment prefers
persistent direct TCP and falls back request-for-request to the exact peer's
signed tracker tunnel. A machine behind NAT therefore contributes without
publishing an unreachable address or opening a data port.

Discovery probes Segment endpoints on its control thread and publishes an
immutable completion estimate with each route. Selection minimizes the sum of
EWMA latency and advertised queue/inflight cost for the complete chain; origin
fallback status is only a tie-breaker. Adding a slower donor therefore does not
replace a faster known route. Expert replicas use the same EWMA model, open a
circuit only after repeated failures, and hedge automatically only after enough
samples show a real p95 tail. `LUMABRI_HEDGE_MS` remains an explicit
override: a positive value is the fixed delay, and `-1` turns hedging off.
These are online estimates, not a speed claim: `swarm_bench.py` on distinct
hosts remains the authority for single-chat and aggregate throughput.
`swarm_rows_bench ADDR LAYER HIDDEN` asks one executor what a layer round
costs with 1, 2, 4, 8 and 16 rows per expert (six experts in parallel, bf16
when the node speaks it): the price of verifying B speculative tokens per
round on that network, before anyone writes a drafter.

The TUI's temperature and top-p are applied to logits returned by Colibri Edge;
temperature zero retains the exact greedy selector. When a compatible replica
exists, a completed turn checkpoints every opaque remote state. If a peer dies,
it opens an exact-range compatible replica, restores the common checkpoint,
replays only the token delta and retries the interrupted batch with the same
conversation.

**Experts run on peers.** For a mixture-of-experts model the chatter keeps only
the dense weights, the router and the KV cache, and sends the 4 KB activation to
the peer that holds each routed expert. Expert weights never reach the chatter.
Both sides are built from the engine's own source, so the local run and the
distributed run are one code path and produce identical tokens. A peer also
advertises its exact build (engine, source hash, ISA, compiler, quantization,
model root), and a chatter refuses a peer whose build differs before it sends a
single activation, because a `-march=native` rebuild can change the last bit and
that must never happen silently. The refusal names the field that diverged, and
the gate splits by what a mismatch means: engine, source hash, model and math
flags always refuse; the OpenMP version never does; compiler and ISA refuse by
default but `LUMABRI_ALLOW_CODEGEN_SKEW=1` downgrades them to a warning and turns
spot-checking on, for a mixed-hardware swarm that accepts verified-but-approximate
results instead of bit-identical ones.

**Peers are not trusted.** Every maintainer computes a sha256 per MiB of what it
holds and sends it with its registration. The origin can sign that truth with an
ed25519 key it keeps offline; the tracker only carries the signature and cannot
mint one, so a chatter verifies every block against a key it holds itself. A
lying peer has its bytes rejected and refetched elsewhere. Remote compute is
checked the only way it can be: `LUMABRI_VERIFY=N` reruns N percent of expert
calls on a second replica and demands identical output. Two honest peers cannot
disagree, so a disagreement is proof of a lie and the run stops.

Prefill and target verification already arrive at the MoE as multiple rows.
lumabri keeps that union intact and sends one multi-row EXEC per selected
expert, including speculative-draft verification; it never serializes a batch
into row-sized requests. `LUMABRI_HEDGE_MS=N` optionally sends a duplicate to
the next replica when the nearest has not replied after N milliseconds and
uses the first valid deterministic result. The fixed delay is deliberately the
public mechanism, not an automatic SLA policy.

## Engines

colibri ships several engines and they do not share a shape, so the expert side
is per engine: a small patch that hooks the MoE function, and an expert-node
binary built from that engine's own source. The engine is never touched, the
patch is applied to a copy, and it is regenerated from source anchors so it
fails loudly instead of applying in the wrong place.

| engine | model | chat | experts on peers |
|---|---|---|---|
| `olmoe` | OLMoE | yes | `expert_node`, proven by `phase2_test.sh` |
| `colibri` | GLM | yes | `expert_node_glm`, proven by `phase2_glm_test.sh` |
| `inkling` | Inkling | yes | `expert_node_inkling`, proven by `phase2_inkling_test.sh` |
| `kimi_k3` | Kimi K3 | yes | `expert_node_kimi`, proven by `phase2_kimi_test.sh` |
| `deepseek` | DeepSeek V4 | yes | `expert_node_deepseek`, proven by `phase2_deepseek_test.sh` |
| `qwen36` | Qwen3.6 | yes | `expert_node_qwen36`, proven by `phase2_qwen36_test.sh` |

"Proven" means the experiment, not the claim: the same engine and the same
prompt, generated twice, once with the experts local and once with every one of
them on a peer, and the tokens compared bit for bit. That test caught a real
bug once. GLM computes an expert over all of its routed rows at once, so feeding
a peer one row at a time gave different floats and the tokens drifted after four
positions. Nothing but running it would have found that.

Build the peers with `make engines`, the patched chat engines with
`make chatters`, or both with `make phase2-all ENGINE=/path/to/colibri/c`, for
the engines your colibri checkout actually has.

### Layer segments

The Segment path keeps whole contiguous layer ranges and their sequence state
resident on peers, reducing the network boundary from one request per
layer/expert to one request per segment. Ordinary `lumabri chat` uses it only
when nobody in the swarm executes experts for the model (or under
`LUMABRI_SEGMENT_REQUIRED=1`): the expert swarm is the primary data plane,
because a machine that donates a few hundred resident experts speeds it up,
while it cannot help a Segment chain at all. GLM, Inkling, Kimi K3, OLMoE, Qwen3.6 and DeepSeek V4 are all release
gates; OLMoE is not a special-case definition of complete. Build details,
relay/failover semantics and the remaining operational boundaries are in
**[SEGMENT_DIRECT.md](SEGMENT_DIRECT.md)**.

## Running a swarm

A full server walkthrough (systemd, firewall, operator key, clients) is in
**[DEPLOY.md](DEPLOY.md)**. The short version:

```sh
make ENGINE=/path/to/colibri/c
make phase2-all ENGINE=/path/to/colibri/c           # classic expert fallback
sudo make install                                    # or PREFIX=$HOME/.local
```

On the server, `lumabri serve --model /srv/model` starts tracker, maintainer,
classic expert executor and the automatic Segment origin. TCP 7300 must be
reachable. A public IPv4 attached to the machine is detected automatically and
enables the fastest direct paths on 7301 onward; allow 7300:7309 when using up
to seven public Segment ranges. Without a reachable address, Segment, READ and
EXEC use signed outbound tracker tunnels, so no data port is required. Add
`--key swarm.key` to sign the model.

`serve` derives names such as `host-gpu-box-7300-storage`, `-experts` and
`-segment-1`; use `--host-name gpu-box` for an operator-chosen prefix. Its live
summary reports connected hosts, bytes served, expert calls and active Segment
runs, so successful swarm work is visible on both sides.

Automatic Segment is a real compute service. It starts only with at least 8 GiB
available by default (`LUMABRI_SEGMENT_MIN_FREE_MB`), gives each sequential
slice a full physical-core team, runs fallback work at low priority and keeps
4 GiB free before accepting a new session. `LUMABRI_RAM_RESERVE_MB` is the
common limit; `LUMABRI_SEGMENT_RAM_RESERVE_MB` and
`LUMABRI_EXPERT_RAM_RESERVE_MB` are per-role overrides. A relay-only/NAT
origin defaults to two sessions per slice; a direct server defaults to four.

Every executor runs the same hysteretic resource governor. `ACTIVE` publishes
capacity; `PRESSURE` and `PAUSED` advertise Segment draining or zero Expert
coverage and reject new work; `RECOVERY` waits three healthy samples before
publishing again. Segment sessions already executing drain through ordinary
pressure, recovery and an operator pause: those transitions must not be
reported to the chatter as a dead peer. Only the critical RAM/swap floor can
cancel an in-flight kernel, after which checkpoint/replay recovery runs.
Expert work already in flight drains, while new calls fall back to another
replica or the Segment's local kernel. Resident RAM stays
allocated inside the donor process, protected by the reserve chosen before
loading, so recovery does not cold-load the model again.
Every transition names its cause and prints available RAM, the configured
reserve and process residency; a `draining` advert is therefore diagnosable
from the executor log instead of being a bare state bit.

On every other machine, pick a role:

| you want to | run |
|---|---|
| chat | `lumabri chat --tracker SERVER:7300` |
| chat on the machine that holds the model | `lumabri chat --local DIR` |
| donate disk (hold bytes) | `lumabri serve --model ./slice --join SERVER:7300 --model-name NAME --donate GB` |
| donate compute | pick `compute` in the join menu; Segment range or experts are selected automatically |

A disk donor is told which complete files to hold, rarest first, by the tracker;
the GB budget is a hard placement input, not permission to fill the disk. A
compute donor needs no model: when Segment is active and a quarter-range fits
after the system reserve, it receives the least-replicated origin range. Direct
P2P is preferred; NAT donors use Segment relay automatically. If the range does
not fit, `--hold auto` sizes a strictly resident expert slice after preserving
the system RAM reserve (`LUMABRI_EXPERT_RAM_RESERVE_MB`, 4096 by default).
The tracker gives it what nobody else covers through direct EXEC or relay and
does not see the donor until every assigned weight is RAM-ready. The origin's
explicit `--cache` executor remains the labelled disk fallback. Both
load through the verified swarm mirror, so the whole model never lands on the
donor.
Automatic compute donation has one machine/user lease shared by `chat` and
`serve --join`. Opening the menu repeatedly therefore cannot start several
executors that each size themselves from the same available-RAM snapshot. A
second process prints the PID/model/tracker of the active owner and may still
donate storage. Stop the owning chat or joined server before replacing it; on
Linux, its executors also receive `SIGTERM` if their parent is killed.
Donors register under `donor-<hostname>-…` (pick one with `--donor-name`);
a name already owned by another peer key is retried with a numbered suffix
instead of being silently rejected forever. So several
compute donors **split the model into disjoint slices automatically**. (`--hold
N` sets the count by hand; `--stride 2:0` / `--stride 2:1` or
`--layers …` split it explicitly if you want to size each machine yourself.)
The origin publishes four stable, layer-aligned fallback ranges by default
(`LUMABRI_SEGMENT_CHUNKS=1..7` overrides it). Each range uses the full local
CPU team because decode traverses the chain serially. A resident Segment donor
then replaces one exact range; it advertises measured RSS only after its engine
has opened, and the server remains the fallback for every range not yet donated.
`LUMABRI_SEGMENT_THREADS=N` is the explicit per-range CPU override.
Within either origin or donor ranges, Segment Hybrid also delegates any fully
covered MoE layer to strict-RAM Expert donors. Selected experts are sent in
parallel; incomplete layers and failed donor calls execute locally. Thus even a
peer too small for a complete Segment range contributes useful resident expert
work, while the server remains the correctness fallback.

Files written by the running engine are deliberately not model content.
Lumabri excludes `.coli_*` state such as `.coli_usage`, KV/checkpoint state and
SSD metadata from storage manifests. These files remain local and mutable;
only checkpoint/model payloads enter signed CAS identity.

Expert executors admit concurrent chatters fairly: when several wait for the
compute gate, the next slot goes to the chatter that has been served least
since the queue last drained, so two chatters on equal networks see the same
tok/s instead of the busier one starving the other. Each executor's stats
line reports compute and queue time per call; the chatter's report shows the
average and the worst layer round, which is what a token actually pays.
Concurrent chats are admitted FIFO at every Segment range. Queue depth and
inflight work are published to the predictive scheduler, the queue is bounded,
and a request that misses its admission deadline returns `BUSY` so an exact
replica can take over instead of waiting behind a 300-second socket timeout.
The four origin ranges form a pipeline, so different chats can occupy different
ranges concurrently. Lumabri deliberately does not fuse rows from unrelated
sessions: current Colibri adapters own opaque per-session state and can round a
cross-session batch differently. Calling that continuous batching before an
engine-neutral multi-session batch ABI exists would break the token oracle.
An activation crosses the wire as bf16 when every one of its values already
is a bf16 (DeepSeek V4 rounds its MoE input and every expert's output to
bf16, so always there; other engines whenever it happens), and an executor
answers in bf16 the same way: half the bytes in each direction on the uplink
that sets a home chatter's tok/s, with the arithmetic untouched, because a
value is never rounded to fit. A replica that holds the expert in RAM is
always preferred to one that streams it from disk, whatever their RTTs: the origin's `--exec-cache`
executor is the last resort, so a donor that loaded an expert receives its
calls the moment it is visible. When two donors hold the *same* expert, the
chatter spreads the load between them by default (set `LUMABRI_SPREAD=0`
for strict nearest-replica routing).
Neither donor needs to know the others exist. Expert requests can fail over to another replica. Stateful
Segment sessions with compatible replicas checkpoint at turn boundaries; on
failure the gateway restores an exact-range replica, replays tokens after the
common checkpoint and retries the interrupted batch. If no compatible replica
exists, it still fails loudly rather than inventing divergent state.

For a manual signing-key rotation, distribute a keyring containing one public
key per line. `--pubkey keyring` and `LUMABRI_PUBKEY=keyring` accept every key
in it (up to 16). First deploy `old+new`, then restart the origin signing with
the new secret, and only after clients and donors have moved remove the old
line. Put the newest key last: the tracker keeps the valid signature made by
the highest-priority (latest) key, so old donor heartbeats cannot roll it back.
Comma-separated public keys are accepted too; the low-level tracker and
maintainer commands also accept repeated `--pubkey`.
There is still one signature per object; the overlap belongs to the verifier,
so the wire format does not change during rotation.

### Optional knobs

Everything works with none of these set. Turn one on by putting it in front of
the command, e.g. `LUMABRI_SPREAD=1 lumabri chat …`.

| set | to |
|---|---|
| `LUMABRI_SPREAD=0` | strict nearest-replica routing (default: spread the load across peers that hold the *same* expert) |
| `LUMABRI_VERIFY=N` | re-run N% of expert calls on a second peer and demand the same answer — a lie stops the run |
| `LUMABRI_HEDGE_MS=N` | after N ms with no reply, ask a second peer too and take whichever comes first |
| `LUMABRI_HEDGE_MS=-1` | never ask a second peer — the only way to get one request per call, which attribution and benchmarking need |
| `LUMABRI_ALLOW_CODEGEN_SKEW=1` | let peers built with a different compiler or CPU join (results are then verified, not bit-identical) |
| `LUMABRI_ENCRYPT=1` | encrypt the transport (see below) |
| `LUMABRI_ENGINE_LOG=path` | append every line the engine prints to this file (the chat otherwise keeps only the tail `/debug` shows) |
| `LUMABRI_SWARM_PATIENCE_S=N` | on a swarm-fed chat, how long a reply waits for a vanished executor before failing plainly (default 600); it never downloads experts to run them locally |
| `V4_DRAFT=N` | on a swarm-fed DeepSeek chat, candidate tokens verified per network round (default 8; `0` turns speculation off); tokens are identical to the one-by-one path, only the number of rounds changes |
| `LUMABRI_EXEC_TIMEOUT_MS=N` | give up on an expert call that has not answered in N ms and fail over (default 120000; the general I/O timeout is five minutes) |
| `LUMABRI_READ_TIMEOUT_MS=N` | give up on a peer that has not delivered an 8 MiB block in N ms and ask the next one (default 60000; the general I/O timeout is five minutes) |
| `RAM_GB=N` | tell a `--local` run how much RAM it may use to keep experts resident |

### Encrypted transport and peer identity

Set `LUMABRI_ENCRYPT=1` on every tracker, maintainer, expert node and chatter
to encrypt tokens, model blocks and activations with an authenticated
X25519/Ed25519 handshake and ChaCha20-Poly1305 frames. If a peer key cannot be
loaded or created, networking fails closed instead of falling back to
plaintext.

Each machine keeps its private endpoint identity in `~/.lumabri/peer.key` and
prints the public half with `lumabri peer-key`. Outbound endpoints are recorded
in `~/.lumabri/known_hosts`; a changed key is refused on later connections.
For first-contact MITM protection, distribute an operator-managed file before
connecting:

```text
SERVER:7300 64_HEX_PEER_KEY
SERVER:7301 64_HEX_PEER_KEY
SERVER:7302 64_HEX_PEER_KEY
```

Then set `LUMABRI_PEER_PINS=/path/to/peer-pins`. Strict pin files must list
every endpoint the process may contact. `LUMABRI_REQUIRE_PIN=1` applies the
same no-learning rule to a preseeded `known_hosts`. The endpoint key is not
the model-signing key: `LUMABRI_PUBKEY` authenticates model contents, while
peer pins authenticate network endpoints. For endpoint-key rotation, publish
two rows for the same address containing old and new keys, switch the server,
then remove the old row. Persistent TOFU instead requires an explicit
`known_hosts` update after verifying the replacement key out of band.

The tracker also persists the first key that owns each maintainer/executor
name. A restart rebuilds placements from heartbeats but does not reopen names
for takeover. Per-key and per-source live-name quotas limit table exhaustion;
they are admission controls, not a claim to solve distributed Sybil attacks.

For the classic expert path, the server also runs an expert node on the whole model, so a fresh swarm works
on day zero with the server executing everything, and donors that join later win
the calls they are nearest for. The nearest replica sets the speed: an expert
held at 2 ms and at 30 ms runs at 10.5 tok/s, not 1.4, because only your closest
copy matters.

## Tests

```sh
make test
```

runs the core suites: byte identity, donor integrity, role parsing, security
(path escape, hostile frame lengths, aggregate receive memory, idle
connections, durable identities and admission quotas), protocol input
validation, cryptographic vectors, encrypted transport and prefetch policy.
Per-engine expert identity runs with fixtures
(`make test-engines`), and DeepSeek V4 against a real model
(`make test-phase2-deepseek MODEL=<dir>`). Assignment, concurrency and signing
have their own scripts (`assign_test.sh`, `concurrency_test.sh`,
`sign_test.sh`). The newer mechanisms have focused targets:
`make test-cas test-key-rotation test-hedge test-relay-exec` and
`make test-segment-v2 test-segment-discovery test-segment-direct-real`.
Segment discovery checks leases, route generations, replicas, draining,
compatibility and the asynchronous snapshot. The direct gate runs all six tiny
families through real TCP executors, matches independent Colibri token oracles,
drives sampled persistent TUI turns, overlaps sessions, and proves rarest-first
automatic range replacement. The same target also forces Segment through a
relay-only NAT topology, kills a selected executor to require checkpoint
restore/replay, and launches the ordinary `serve` + `chat` UX without a public
data address. Relay EXEC needs
an OLMoE engine source under `ENGINE`; its script reports `SKIP` explicitly
when that external checkout is absent. Every claim in this README has a script
behind it.

## How it compares

Peer-to-peer LLM inference exists; this combination does not.
[Petals](https://github.com/bigscience-workshop/petals) and llama.cpp RPC split
consecutive transformer layers across devices, which needs each slice to run
fast, in practice a GPU. lumabri splits at expert granularity instead, which
matches MoE sparsity: only 4 KB travels per expert, a peer is useful holding a
single one, and a swarm with no GPU is a working swarm. The output is
byte-identical by construction, because remote and local are the same code, which
is also what makes spot-check verification of untrusted peers possible at all.

## Requirements

Linux, gcc, GNU make, and Python 3 (it applies the chatter hooks to the engine
by anchor at build time, so a patch survives colibri moving the code around it;
numpy is only needed for the synthetic test fixtures). A
[colibri](https://github.com/JustVugg/colibri) build provides the engine
binaries.

## Status

Working prototype, deployable. Open swarms verify bytes (sha256 per MiB and a
signed complete-model root, checked by the chatter against its own trust set)
and results (spot-check on a second replica). Private swarms add an invite token
everywhere with `LUMABRI_TOKEN`; `LUMABRI_ENCRYPT=1` protects that token and
activations in transit, using persistent TOFU or strict endpoint pins.
Multi-row speculative verification, fixed-delay
hedging, a local cross-checkpoint CAS, manual old+new key rotation and NAT relay
for both READ and EXEC are implemented. Automatic SLA tuning, distributed/S3
CAS, KMS/HSM integration and automatic revocation are intentionally not part of
this dependency-free base. Expert execution is checked by replica agreement,
not by the operator signature.

## License

Apache 2.0
