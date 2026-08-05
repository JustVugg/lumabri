# lumabri

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
engine on the fly. Several machines can serve different models to one
tracker with `lumabri serve --model DIR --join TRACKER:PORT`.

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
verifies byte identity cold, warm, and with every peer dead.

## Phase 2: peers execute experts

The second half of the project, prototyped and measured: the chatter keeps
only the dense weights, router and KV cache, and sends the activation row
(4 KB) to the peer that holds each routed expert. Expert weights never
reach the chatter. Both sides are built from the engine's own source
(`expert_node.c` includes `olmoe.c`), so local and remote runs are one code
path and produce identical tokens.

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

**Private swarm — invitation only.** Set `LUMABRI_TOKEN=S` on every
machine: `serve` passes it to its tracker, and the tracker, every
maintainer and every expert node refuse unauthenticated connections. The
token guards the bytes and the compute, not just the index.

`./phase5_test.sh` proves all three open-swarm defences with peers that lie
exactly as an adversary would (honest manifest, corrupt bytes): 7 corrupt
blocks rejected with the mirror still byte-identical, a poisoner stripped
at registration, and a lying executor caught on the first spot-check.

## Deploy

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
| donate disk (hold bytes) | `lumabri serve --model ./slice --join SERVER:7300 --model-name NAME --donate GB` |
| donate compute (execute experts) | `expert_node --model DIR --tracker SERVER:7300 --cache N` |

## Layout

| file | role |
|------|------|
| `lumabri.c` | the front end: `serve` and `chat` |
| `tracker.c` | index of who holds what, no model bytes |
| `maintainer.c` | serves byte ranges of a model directory |
| `lumashim.c` | the chatter-side LD_PRELOAD shim |
| `lumabri_proto.h` | binary wire protocol, header only |
| `expert_node.c` | phase 2 peer: holds experts and executes them |
| `lumabri_client.h` | phase 2 chatter side |
| `selftest.sh` | byte identity: cold, warm, offline |
| `phase2_test.sh`, `phase2_bench.sh` | phase 2 correctness and benchmark |
| `phase3_test.sh` | proximity, readahead, failover — measured |
| `phase4_test.sh` | SSD cache, tracker discovery, delegate & fall back |
| `phase5_test.sh` | integrity: lying peers caught, poison stripped |
| `lumabri_sha.h` | sha256, self-contained — the root of the trust chain |
| `make_tiny_olmoe.py` | synthetic OLMoE-shaped fixture for tests |

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

Working prototype, deployable. Open swarms verify bytes (sha256 per MiB,
truth from the tracker) and results (spot-check on a second replica);
private swarms need an invite token everywhere. Not yet done, in order of
importance: signing the tracker's ground truth so a compromised tracker
cannot rewrite it (today the root of trust is the operator's server),
speculative drafting with batch-union (the remaining multiplier against WAN
latency), hedged requests against stragglers, tracker-side expert
assignment, NAT hole punching.

## License

Apache 2.0
