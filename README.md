<img src="logo.svg" alt="lumabri — tiny engine, immense swarm" width="524">

Run huge mixture-of-experts models from a swarm of peers, with the
[colibri](https://github.com/JustVugg/colibri) engine. Pure C, no
dependencies.

One machine shares a model. Any other machine chats with it: nothing is
downloaded up front, the bytes an inference actually touches arrive from the
peer on first use and stay in a local mirror. The second question is served
from local disk at full speed. The engine binary is unmodified.

The founding principle: any machine may join, GPU or not. The engine was
built for CPU and SSD first; a GPU only makes it faster, never different,
and the output is byte-identical either way. A swarm with no GPU at all is
a working swarm. Networks that pool GPUs recruit from the few; lumabri
recruits from everyone.

## Quick start

Build:

```sh
make
```

On the machine that has a model (any colibri model directory):

```sh
./lumabri serve --model /path/to/model
```

On the machine that wants to chat (needs a colibri build for the engine):

```sh
./lumabri chat --tracker <server-ip>:7300 --engines-dir /path/to/colibri/c
```

That is all. The first answer is slower while the working set crosses the
network; afterwards the mirror in `~/.lumabri` keeps serving even if the
server goes offline.

In the chat: `/swarm` shows the network live and anonymous (peers are
numbered, never named: model held, GB, bytes served, heartbeat), `/model`
lists the models on the swarm and switches between them, restarting the
engine on the fly.

### For someone who only ever opens the TUI

```sh
lumabri
```

No arguments. It asks for the swarm's address and, once, for the operator's
public key; it finds the engines itself; and it remembers all of it in
`~/.lumabri/config`, so the second time it is Enter, Enter, and you are in.

That matters more than convenience. Everything it asks for used to be a flag,
and getting one wrong does not produce "invalid argument" — a missing
`--engines-dir` produces a 299 GB download, and a missing key produces a
model nobody verified. Flags still win when given, so a script never
inherits somebody's saved answers.

### Joining: chat, or bring something

`lumabri chat` asks once, on the way in, and Enter means "just chat" so the
impatient path is one key:

```
  come entri nello sciame?

    1  solo chattare        non condividi niente
    2  chatti e doni disco  tieni un pezzo di glm per lo sciame
    3  chatti e doni calcolo  esegui esperti per gli altri
    4  tutti e due

  invio = solo chattare
```

Pick 2 and it asks how many GB (Enter takes a quarter of the free space,
capped), then starts a maintainer with that budget: the tracker assigns it
the least-replicated files first, it pulls them verifying every byte against
the operator's signature, and serves them. Pick 3 and it starts the expert
node for that model's engine. Both run as children of the chat and stop when
you close it — which is the honest lifetime for something offered from a
terminal you have open. A donor that should outlive the session is
`lumabri serve --join`.

Donating compute needs the model on your disk (an expert node reads the
weights from there), so option 3 is offered only with `--model-dir DIR`.
Donating disk needs nothing: you start empty and the swarm fills you.

Scripts skip the question: `--role chat|disk|compute|all`, with `--donate GB`
and `--model-dir DIR`.

### Several models on one swarm

One tracker is an index, not a model server, so it holds as many models as
you point at it — from one machine or many:

```sh
lumabri serve --model /models/glm      --port 7300
lumabri serve --model /models/olmoe    --join 127.0.0.1:7300 --port 7310
lumabri serve --model /models/deepseek --join 127.0.0.1:7300 --port 7320
```

Each `serve` brings its own maintainer (the bytes) and its own expert node
(the compute), and each registers under its model's name. A chatter sees
`3 modelli sullo sciame` and switches with `/model <name>`: the engine is
restarted against the new model, and since the engine binary is chosen from
that model's `model_type`, switching between different *architectures* works
too — GLM to OLMoE to DeepSeek, one client, one tracker.

Donors are per model as well: a machine can hold a slice of one model and
execute experts for another. The tracker keeps them apart, and a chatter
only ever discovers the peers for the model it is talking to.

Donating space, the server decides: a machine with an empty directory can
offer a byte budget and the tracker assigns it the slice to hold,
rarest-first, so every donated gigabyte lands where the swarm is thinnest.
The donor pulls its slice from the swarm, then serves it.

```sh
./lumabri serve --model ./slice --join TRACKER:7300 \
                --model-name tiny_olmoe --donate 5
```

NAT floor: maintainers keep one outbound control connection to the tracker
and, when a direct dial fails, bytes are relayed through it. A peer behind
any home NAT serves with zero router configuration; direct peer-to-peer
stays the first choice. The selftest proves byte identity on the relay path
too. Private swarms: set `LUMABRI_TOKEN=S` on every machine — `serve`
passes it to its tracker, and both the tracker and every maintainer refuse
unauthenticated connections, so the token guards the bytes, not just the
index. A maintainer advertising
localhost from another machine gets its address corrected by the tracker to
what the connection shows.

## Tutorial: a first swarm in five minutes

No model at hand? Generate the tiny synthetic one (python3 + numpy) and
serve it — every step below is the real thing, just small.

```sh
make && make fixture              # builds everything + tiny_olmoe/
./lumabri serve --model ./tiny_olmoe
```

In a second terminal, the chatter. It needs the engine binaries from a
[colibri](https://github.com/JustVugg/colibri) build:

```sh
./lumabri chat --engines-dir /path/to/colibri/c
```

What you should see, in order:

1. The engine boots against a directory that does not exist on the
   chatter's disk: the config and tokenizer arrive from the swarm on
   first touch.
2. Ask something. The first answer is the slow one — watch the `net MB`
   counter climb while the working set crosses the wire.
3. Ask again. `mirror caldo, zero rete`: the second answer is served from
   `~/.lumabri/<model>/cache` at local speed.
4. `/swarm` shows the network, anonymous; `/model` lists what else it holds.
5. Kill the serve terminal and keep chatting: the warm mirror answers with
   every peer dead. That is pass 3 of `make test`, lived instead of read.

To grow it, a friend on another machine chats with
`--tracker <your-ip>:7300`, or donates disk so the model survives you
turning your machine off:

```sh
./lumabri serve --model ./slice --join <your-ip>:7300 \
                --model-name tiny_olmoe --donate 2
```

The tracker assigns the donor the least-replicated files first; the donor
pulls them from the swarm, then serves them. `/swarm` in your chat now
shows two peers.

## Trying it, for real

Two stages: everything on one machine first, then more machines. Nothing
below is a simulation — the single-machine version runs the same binaries
over the same sockets.

**On the machine with the model.** Build both halves of phase 2 (`chatters`
is the patched engines, `engines` is the expert nodes) and check the model
runs at all before any network is involved:

```sh
make phase2-all ENGINE=/path/to/colibri/c && sudo make install
lumabri chat --local /path/to/model --engines-dir /path/to/colibri/c
```

Then the swarm, still on one machine:

```sh
lumabri key --out swarm                       # once, keep swarm.key safe
lumabri serve --model /path/to/model --key swarm.key --exec-cache 256
```

For a swarm anyone else can reach, add `--advertise <the machine's public
IP>`: peers publish the address they are given, and without it they publish
`127.0.0.1` — right for this machine, useless for everyone else. `serve`
says so loudly if you forget, because the failure mode is a remote chatter
that falls back to the relay and never turns phase 2 on, which reads as
"slow" rather than "misconfigured".

Expect, in order: the hashing progress (first start only — minutes on a big
model), `ORIGIN: signed the truth of N files`, and `serving EXEC on :7302 …
registered with tracker`. In a second terminal:

```sh
LUMABRI_PUBKEY=$(cat swarm.pub) lumabri chat --tracker 127.0.0.1:7300     --engines-dir /path/to/colibri/c
```

**The line to look for is `[lumabri] phase 2 active`.** Without it the
chatter is running the stock engine and will download expert weights instead
of asking peers to run them — `du -sh ~/.lumabri` is the other tell: with
phase 2 on it grows by the dense part and stops.

**Adding machines.** Open 7300-7302 (and +10 per extra model) in the
firewall — on a cloud host, in the provider's console *as well as* in `ufw`.
Every other machine needs lumabri and a colibri checkout, then
`make phase2-all ENGINE=…`. From there the three roles:

| they want to | command |
|---|---|
| chat | `LUMABRI_PUBKEY=<pub> lumabri chat --tracker IP:7300 --engines-dir …` |
| chat + donate disk | the same, then pick 2 (or `--role disk --donate 50`) |
| donate compute | needs the model locally: `--role compute --model-dir DIR`, or `expert_node_<engine>` as a service |

`/swarm` in any chat shows who arrived. The proof that the swarm is really
carrying the work: while a reply is generating, kill a donor — you get one
failover line and the tokens continue, identical.

### Several people at once

`concurrency_test.sh` runs the same generation from N chatters simultaneously
and reports the spread between the fastest and the slowest, because "does it
answer" is the easy question and "does anyone get starved" is the real one.
On one 6-core box, tiny_olmoe, everything (server, peers, clients) sharing
those cores:

| chatters | fastest | slowest | spread |
|---|---|---|---|
| 1 | 1.0 s | 1.0 s | 0.0 s |
| 2 | 1.7 s | 1.8 s | 0.0 s |
| 4 | 10.4 s | 10.7 s | 0.3 s |

Nobody is starved — the spread stays flat while the absolute time grows,
which is what CPU contention looks like and not what a lock convoy looks
like. Four clients on six cores that are already running the server and its
expert node is oversubscription, and on separate machines the clients bring
their own cores.

Where contention actually lives, so those numbers can be read honestly:

- **bytes scale.** The maintainer answers reads with positional `pread` on
  shared fds — no lock on the read path — and the page cache serves every
  client the same hot bytes.
- **hot experts scale, cold ones queue.** The expert node runs a thread per
  connection, but a cache *miss* holds a single loader lock, because the
  engine loaders are engine-internal state and not re-entrant. Size
  `--cache` so the working set fits and misses are rare; that is the knob.
- **the tracker is not on the hot path at all.** Expert discovery is checked
  at most once every 5 s by default, while activations still travel directly
  between chatter and expert peers. `LUMABRI_DISCOVERY_MS` changes that
  interval.

## How it works

`serve` runs two small programs: a tracker, which is only an index of who
holds which files, and a maintainer, which answers byte-range reads on the
model directory. A maintainer can hold just a slice of a model
(`--include` patterns), and several maintainers can share one model.

`chat` mounts the model through `liblumabri.so`, an `LD_PRELOAD` shim that
interposes exactly the libc calls the engines use on a model directory
(`open`, `fopen`, `opendir`, `pread`). Files are mirrored as sparse local
files of the true size, so `fstat`, `readdir` and the kernel page cache work
natively. A missing block is fetched from a peer, written to the mirror,
then the engine's own `pread` proceeds. A warm read costs one table lookup
plus a normal local read: there is no FUSE and no daemon on the read path.

Correctness rule, inherited from colibri: the network may only change where
bytes come from, never which bytes. Writing to model files returns `EROFS`.
A block no peer can serve is a loud `EIO`, never silent zeros. The selftest
verifies byte identity cold, warm, and with every peer dead. The data file is
synced before its bitmap bits are committed in a background batch, and every
bitmap is bound to the complete model root. This avoids an `fdatasync` per MiB
while ensuring a crash or same-size checkpoint replacement cannot bless stale
bytes as cached. Concurrent chatters share the same mirror under a checkpoint
lock: ordinary reads remain concurrent, reset is exclusive, and persisted
bitmaps are merged so one process cannot erase another process's warm blocks.

## Phase 2: peers execute experts

The second half of the project, prototyped and measured: the chatter keeps
only the dense weights, router and KV cache, and sends the activation row
(4 KB) to the peer that holds each routed expert. Expert weights never
reach the chatter. Both sides are built from the engine's own source
(`expert_node.c` includes `olmoe.c`), so local and remote runs are one code
path and produce identical tokens.

An expert manifest also commits to the engine family, content fingerprint of
the engine source and generated patch, compiler and ISA profile, effective
quantization, tensor shape, model name, and complete model root. A chatter
refuses an incompatible peer before the first activation is sent. This keeps
`-march=native`, a changed checkout, or another checkpoint from creating a
mixed-arithmetic swarm that only drifts several tokens later.

Measured on one machine with pinned cores and network emulated in the peer
(`phase2_bench.sh`, details and limits in `RESULTS_PHASE2.md`):

| scenario                       | tok/s | chatter RSS |
|--------------------------------|-------|-------------|
| local, experts resident in RAM | 1.92  | 2.53 GB     |
| local, experts from disk       | 0.04  | 1.12 GB     |
| P2P, gigabit LAN               | 5.97  | 1.04 GB     |
| P2P, internet at 30 ms         | 1.13  | 1.04 GB     |

Identical tokens on every path. On a LAN the network costs 15 percent; on
the internet the sequential layers are the wall. This build needs the small
patch in `engine_patches/olmoe-p2p.diff` applied to colibri, then:

```sh
make phase2 ENGINE=/path/to/colibri/c
./phase2_bench.sh
```

### More than one engine

colibri ships several engines and they do not share a shape, so phase 2 is
per engine: a patch that hooks the MoE function, and an expert-node binary
built from that engine's own source. Everything engine-specific lives in one
header per engine under `expert_engines/`; `expert_node.c` itself is
engine-agnostic. The patches are generated from source anchors by
`engine_patches/make_patches.py`, so they can be regenerated against any
colibri version and fail loudly instead of applying to the wrong place. The
engine is never modified — the test patches a copy.

All five engines **chat** through lumabri: phase 1 serves any model's bytes,
and the front end speaks both engine dialects. Phase 2 — peers executing the
experts — is the part that is per engine.

| engine | model | chat (phase 1) | experts on peers (phase 2) |
|---|---|---|---|
| `olmoe` | OLMoE | yes | `expert_node` — **proven**, `phase2_test.sh` |
| `colibri` | GLM | yes | `expert_node_glm` — **proven**, `phase2_glm_test.sh` |
| `inkling` | Inkling | yes | `expert_node_inkling` — **proven**, `phase2_inkling_test.sh` |
| `kimi_k3` | Kimi K3 | yes | `expert_node_kimi` — **proven**, `phase2_kimi_test.sh` |
| `deepseek` | DeepSeek V4 | yes | `expert_node_deepseek` — **proven**, `phase2_deepseek_test.sh` |

`make test-engines` runs the four with fixtures; DeepSeek needs a real model
(`make test-phase2-deepseek MODEL=<dir>`). "Proven" means the experiment, not
the opinion: the same engine, the same prompt, generated twice — once with the
experts local and once with every one of them on a peer — and the tokens
compared. Each needs a fixture, so this repo carries a generator per engine
(`make_tiny_olmoe.py`, `make_tiny_inkling.py`, `make_tiny_kimi.py`; GLM uses
colibri's own `glm_tiny_i4`). They are numpy-only and build random-weight
models: the output is gibberish, which is the point — what is being measured
is that two runs agree bit for bit, not that the model is any good.

The fixtures are not ceremony. GLM's first version looked right, compiled,
ran, and produced tokens that matched for four positions and then drifted,
because GLM computes an expert over all its routed rows at once and the
peers were being fed one row at a time. Nothing but running it would have
caught that.

**DeepSeek V4** was the one that looked easiest from the outside — it has a
clean public API (`coli_v4_engine_open`, `coli_expert_lookup`,
`coli_v4_expert_forward_ref`) where the others need their internals — and
turned out to have the sharpest edge:

- **the router weight is not a scale.** `coli_v4_expert_forward_ref` folds it
  in *before* the down projection and rounds the product to bf16, so
  `w · expert(x)` is not what the engine computes. It has to travel with the
  activation and be applied by the peer; the chatter-side multiply that every
  other engine allows would have looked right and been wrong. `EXEC` carries
  the weights when the body is long enough to hold them.
- **three places apply a target expert** — one per token, one in the block
  pipeline, one in the batch union — and each had to be hooked. Missing one
  is not a crash; it is a layer that quietly runs locally. The dspark draft
  path is deliberately left alone: those are the *draft* model's experts, and
  the target verifies every token they propose.
- **`deepseek.c` is generated** by `tools/amalgamate_deepseek.py`, so the
  patch is regenerated from anchors like the rest, and the build renames its
  CLI entry point in a copy because the file `#undef`s `main` halfway through.

Its peer is also the only one that holds no dense weights at all: V4's expert
store opens straight off the model directory, which is what a peer should
have wanted all along.

There is no synthetic fixture for it — V4 validates a strict config and an
FP8/FP4 tensor plan, so a random-weight stand-in is more work than it is
worth. `phase2_deepseek_test.sh` runs against a real model instead, and the
engine scores itself: the local run writes an oracle, the P2P run is checked
against it by `--oracle`.

Three shapes had to be taught to the client, and they are worth naming
because they are what "support another engine" actually costs:

- **not every layer routes.** Dense first layers, and for GLM an MTP row at
  index `n_layers` that does. `lumi_init_ex` takes the mask; without it the
  non-existent experts of dense layers count as missing and phase 2 silently
  stays off on every model that has one.
- **batching is part of the arithmetic.** GLM gathers every row a layer
  routed to an expert and computes them together; nr rows in one call is not
  nr calls of one row. EXEC carries a row count, and `lumi_moe_apply_batch`
  reproduces the engine's own union, row order and accumulation order.
- **the experts do not always live in hidden space.** Kimi K3 routes in a
  latent of `c->latent`, so that is the width on the wire.

## Phase 3: the war on RTT

The swarm is as fast as your *nearest replica*, not your average peer —
so lumabri measures distance and acts on it, with no coordination: every
node probes its own peers at startup (two PINGs, keep the min) and the
tracker stays a Napster index that never knows where anyone is.

- **Nearest replica first** (both phases): peers within 25% + 2 ms of the
  best are "equally near" and share the load; everyone farther is failover,
  the relay last. Measured: an expert replicated at 2 ms and at 30 ms runs
  at **10.5 tok/s instead of 1.4** — the 30 ms wall is the wall of your
  nearest replica only.
- **Readahead** (`LUMABRI_PREFETCH`, default 2 blocks): while the engine
  chews on block N the swarm is already sending N+1..N+K. Measured: a cold
  mirror on a 40 ms swarm loads **45% faster**.
- **Replica failover** (phase 2): a peer that dies mid-generation costs one
  retry on the next replica, not the conversation. Measured: peer killed
  while generating, 4 failovers, tokens byte-identical. An expert with no
  live replica stays a hard error — never a silent local fallback.

`./phase3_test.sh` proves all three, no root needed (distance is emulated
inside the serving peer). Still on the table, engine-side: speculative
drafting with batch-union (one layer round per multi-token draft) and
expert prediction — the multipliers that stack on top of proximity.

## Phase 4: bootstrap and delegate

A swarm has to work on day zero, when nobody has donated anything yet. The
policy: **the server executes first, delegates as donors arrive, and stays
the fallback of last resort.**

- `lumabri serve` also runs an expert node on the whole model (when the
  family has one; `--no-exec` disables). Experts stay **on the SSD** and
  stream through an LRU RAM cache (`--cache N` slots, the colibri way): a
  16 GB machine can hold a 500 GB slice, hot experts cost RAM speed, a cold
  one costs one NVMe read. Byte-identity survives streaming — proven with a
  cache so small that 80% of calls were cold loads.
- Chatters need **zero expert configuration**: the engine asks the tracker
  who can execute for the model (expert nodes heartbeat like maintainers)
  and runs phase 2 against whoever answers — on a fresh swarm, that is the
  server alone.
- A donor joins with `expert_node --model DIR --tracker H:P [--cache N]
  [--stride N:OFF]`, is discovered, and wins the calls it is nearest for.
  Discovery continues during generation, so no chatter restart is needed.
  If it dies mid-generation the call fails over — replica, then a fresh
  tracker query, ultimately the server. If the swarm cannot cover every
  expert at startup, phase 2 simply stays off and the engine runs experts
  itself from the phase-1 mirror. Every rung of that ladder is loud; none
  of them can change a byte.

`./phase4_test.sh` proves all three claims, including a donor killed
mid-generation with tokens byte-identical to the local reference.

## Phase 5: two kinds of swarm

**Open swarm — anyone may join, nobody is trusted.** Integrity is a chain
of custody rooted at the swarm operator, never at the peer serving the
bytes:

- Every maintainer computes a **sha256 per MiB** of what it holds (cached
  in `.lumabri_hashes/`, so only the first start pays) and sends them with
  its registration.
- The tracker keeps the **first** announcement of each file as ground truth
  — the origin registers before any donor exists — and **strips the files
  of any later registrant whose hashes disagree**: poison dies at the index
  and never reaches a placement.
- Chatters and pulling donors ask the *tracker* for the truth and verify
  every fetched block. A peer that lies gets its bytes rejected and the
  block refetched elsewhere, loudly. `LUMABRI_REQUIRE_HASH=1` refuses to
  fetch at all where integrity data is missing — the strict mode for
  swarms of strangers.
- Phase 2 results are checked the only way untrusted compute can be:
  `LUMABRI_VERIFY=N` reruns N% of expert calls on a **different replica**
  and demands byte-identical output. Determinism is what makes this work —
  two honest peers cannot disagree, so a disagreement is proof of a lie and
  the run stops rather than emit a token nobody can vouch for.

**Signed swarm — not even the tracker is trusted.** Hashes alone make the
tracker the authority: it decides which bytes are true, so compromising it
rewrites the model. A signature moves that authority to a key the operator
keeps offline.

```sh
lumabri key --out swarm                 # ed25519 keypair, secret is 0600
lumabri serve --model /srv/model --key swarm.key
LUMABRI_PUBKEY=<the 64 hex chars> lumabri chat --tracker HOST:7300
```

The origin signs each file's hash vector (bound to model, path, chunk size
and file size, under a domain tag, so a signature cannot be replayed onto
another file). It separately signs one canonical root over the complete file
inventory, paths, sizes, and every per-MiB hash. That root binds phase 1 and
phase 2 to one checkpoint and invalidates a warm mirror even when a replaced
file has the same size. The tracker stores and forwards the signatures and — given
`--pubkey`, which `serve --key` passes automatically — refuses any claim
that is not signed. The chatter rebuilds the signed message itself and
checks it against the key it obtained **out of band**: a compromised
tracker can withhold the truth, but it cannot rewrite it and be believed.
Carrying a public key implies strict mode: unsigned bytes are refused, not
merely noted. The Ed25519 and SHA-512 here are self-contained C, checked
against RFC 8032 and against OpenSSL in both directions by `sign_test.sh`.

**Private swarm — invitation only.** Set `LUMABRI_TOKEN=S` on every
machine: `serve` passes it to its tracker, and the tracker, every
maintainer and every expert node refuse unauthenticated connections. The
token guards the bytes and the compute, not just the index. It composes
with signing: the token says who may connect, the key says which bytes are
real.

`./phase5_test.sh` proves all three open-swarm defences with peers that lie
exactly as an adversary would (honest manifest, corrupt bytes): 7 corrupt
blocks rejected with the mirror still byte-identical, a poisoner stripped
at registration, and a lying executor caught on the first spot-check.

## Deploy

A full walkthrough for a real server — Hetzner, systemd, firewall, operator
key, clients — is in **[DEPLOY.md](DEPLOY.md)**. The short version:

```sh
make && make phase2 ENGINE=/path/to/colibri/c    # phase2 optional
sudo make install                                 # or PREFIX=$HOME/.local
```

On the server: `lumabri serve --model /srv/model --port 7300` (open TCP
7300-7302: tracker, maintainer, executor). A systemd unit is one stanza:
`ExecStart=/usr/local/bin/lumabri serve --model /srv/model`,
`Restart=always`, plus `Environment=LUMABRI_TOKEN=...` for a private swarm.

On every other machine, pick a role — or several:

| you want to | run |
|---|---|
| chat | `lumabri chat --tracker SERVER:7300` |
| chat on the machine that holds the model | `lumabri chat --local DIR` (no mirror, no second copy) |
| donate disk (hold bytes) | `lumabri serve --model ./slice --join SERVER:7300 --model-name NAME --donate GB` |
| donate compute (execute experts) | `expert_node<engine> --model DIR --tracker SERVER:7300 --cache N` |

The compute peer is per engine, because the engines do not share an expert
shape. `lumabri serve` picks the right one from the model's `model_type`;
donating by hand means naming it:

| model | binary | notes |
|---|---|---|
| OLMoE | `expert_node` | |
| GLM | `expert_node_glm` | `--bits` must match the chatter's (default 8) |
| Inkling | `expert_node_inkling` | `--bits` likewise |
| Kimi K3 | `expert_node_kimi` | activations are latent-width, not hidden |
| DeepSeek V4 | `expert_node_deepseek` | holds no dense weights: `--cache` is expert slots per layer |

A compute donor says only what it knows about itself — `--hold N`, how many
experts it can carry — and the tracker answers with the set nobody else
covers, rarest first. It used to need `--stride 9:3`, which means knowing how
many other donors exist and which index is free: coordination, in a thing
built to remove coordination. `--layers`/`--stride` still work and simply opt
out. `assign_test.sh` starts three nodes that know nothing of each other and
checks they end up disjoint, cover the whole set, and keep their slice across
a restart.

`make engines` builds the peers, `make chatters` the patched engines, and
`make phase2-all` both — for the engines your colibri checkout actually has.
`deepseek.c` is generated by colibri's `tools/amalgamate_deepseek.py` and is
absent from a fresh clone, so that one is skipped with a line saying so
rather than failing the build for everyone who only wants GLM.

## Layout

| file | role |
|------|------|
| `lumabri.c` | the front end: `serve` and `chat` |
| `tracker.c` | index of who holds what, no model bytes |
| `maintainer.c` | serves byte ranges of a model directory |
| `lumashim.c` | the chatter-side LD_PRELOAD shim |
| `lumabri_proto.h` | binary wire protocol, header only |
| `expert_node.c` | phase 2 peer: holds experts and executes them |
| `expert_engines/*.h` | one per engine: the only engine-shaped code |
| `engine_patches/make_patches.py` | generates the engine patches from source anchors |
| `lumabri_client.h` | phase 2 chatter side |
| `selftest.sh` | byte identity: cold, warm, offline, NAT relay, same-size checkpoint replacement |
| `donate_test.sh` | an empty disk donor pulls a slice and outlives the origin |
| `signed_donor_test.sh` | a donor on a signed swarm verifies, republishes the signature, and refuses the wrong key |
| `role_test.sh` | `--role` takes whole words: chat is not compute |
| `expert_input_test.sh` | phase-2 peers reject out-of-range network indices |
| `phase2_test.sh`, `phase2_bench.sh` | phase 2 correctness and benchmark (olmoe) |
| `phase2_glm_test.sh`, `phase2_inkling_test.sh`, `phase2_kimi_test.sh`, `phase2_deepseek_test.sh` | phase 2 byte identity, one per engine |
| `phase3_test.sh` | proximity, readahead, failover — measured |
| `phase4_test.sh` | SSD cache, tracker discovery, delegate & fall back |
| `phase5_test.sh` | integrity: lying peers caught, poison stripped |
| `sign_test.sh` | sha512/ed25519 vs RFC 8032 and OpenSSL, signed swarm |
| `security_test.sh` | path escape, hostile frame lengths, and bounded idle connections |
| `assign_test.sh` | three uncoordinated compute donors split the experts by themselves |
| `concurrency_test.sh` | N chatters at once: does anyone get starved |
| `chat_proto_test.sh` | both engine dialects, and a dying engine that explains itself |
| `DEPLOY.md` | server walkthrough: Hetzner, systemd, keys, clients |
| `lumabri_sha.h` | sha256, self-contained — per-block integrity |
| `lumabri_sign.h` | sha512 + ed25519 — the operator's authority |
| `make_tiny_olmoe.py` | synthetic OLMoE-shaped fixture for tests |
| `make_tiny_inkling.py`, `make_tiny_kimi.py` | the same for Inkling and Kimi K3, numpy only |
| `expert_engines/deepseek.h` | the V4 peer: expert store only, no dense weights |

## Related work

Peer-to-peer LLM inference exists; this combination does not.
[Petals](https://github.com/bigscience-workshop/petals) runs dense models
BitTorrent-style by assigning consecutive transformer blocks to volunteer
GPUs; [hivemind's decentralized MoE](https://arxiv.org/abs/2002.04013)
spread experts over volunteers, but for training; exo and llama.cpp's RPC
mode split layers across devices one person owns; MeshLLM hands out static
layer ranges. lumabri differs on three axes at once:

| axis | layer-sharding swarms | lumabri |
|------|----------------------|---------|
| granularity | a slab of consecutive layers | one expert — matches MoE sparsity, 4 KB activations travel, a peer is useful holding a single expert |
| hardware floor | a dense slice must run fast: in practice a GPU | any machine; a swarm with no GPU is a working swarm |
| determinism | best-effort, outputs vary across hardware | byte-identical by construction: remote and local are one code path |

The third axis is not cosmetic: deterministic output is what makes
spot-check verification of untrusted peers possible at all — two honest
peers must agree to the byte, so a lie is detectable by sampling.

## Requirements

Linux, gcc, GNU make. Python 3 with numpy only for generating the test
fixture. A [colibri](https://github.com/JustVugg/colibri) build provides the
engine binaries.

## Status

Working prototype, deployable. Open swarms verify bytes (sha256 per MiB and
a signed complete-model root, checked by the chatter against a key it holds
itself) and results (spot-check on a second replica); private
swarms need an invite token everywhere. Not yet done, in order of
importance: speculative drafting with batch-union (the remaining multiplier
against WAN latency), hedged requests against stragglers, a content-addressed
mirror shared across checkpoints, key rotation and revocation, NAT hole
punching. Expert
execution is not yet covered by the operator signature — a peer's results
are checked by replica agreement, not by a key.

## License

Apache 2.0
