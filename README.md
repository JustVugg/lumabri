<img src="logo.svg" alt="lumabri: tiny engine, immense swarm" width="524">

Run huge mixture-of-experts models from a swarm of peers, with the
[colibri](https://github.com/JustVugg/colibri) engine. Pure C, no dependencies.

One machine shares a model. Any other machine chats with it. Nothing is
downloaded up front: the bytes an inference actually touches arrive from a peer
on first use and stay in a local mirror, so the second question is served from
local disk at full speed. The engine binary is never modified.

Any machine may join, GPU or not. The engine was built for CPU and SSD first; a
GPU only makes it faster, never different, and the output is byte-for-byte the
same either way. Networks that pool GPUs recruit from the few. lumabri recruits
from everyone.

## Quick start

```sh
make
```

On the machine that has a model (any colibri model directory):

```sh
./lumabri serve --model /path/to/model
```

On a machine that wants to chat (it needs a colibri build for the engine):

```sh
./lumabri chat --tracker <server-ip>:7300 --engines-dir /path/to/colibri/c
```

That is all. The first answer is slower while the working set crosses the
network. Afterwards the mirror in `~/.lumabri` keeps serving even if the server
goes offline.

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

Inside the chat, `/swarm` shows the network live and anonymous (peers are
numbered, never named), and `/model` lists the models on the swarm and switches
between them on the fly.

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

**Experts run on peers.** For a mixture-of-experts model the chatter keeps only
the dense weights, the router and the KV cache, and sends the 4 KB activation to
the peer that holds each routed expert. Expert weights never reach the chatter.
Both sides are built from the engine's own source, so the local run and the
distributed run are one code path and produce identical tokens. A peer also
advertises its exact build (engine, source hash, ISA, compiler, quantization,
model root), and a chatter refuses a peer whose build differs before it sends a
single activation, because a `-march=native` rebuild can change the last bit and
that must never happen silently.

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

"Proven" means the experiment, not the claim: the same engine and the same
prompt, generated twice, once with the experts local and once with every one of
them on a peer, and the tokens compared bit for bit. That test caught a real
bug once. GLM computes an expert over all of its routed rows at once, so feeding
a peer one row at a time gave different floats and the tokens drifted after four
positions. Nothing but running it would have found that.

Build the peers with `make engines`, the patched chat engines with
`make chatters`, or both with `make phase2-all ENGINE=/path/to/colibri/c`, for
the engines your colibri checkout actually has.

## Running a swarm

A full server walkthrough (systemd, firewall, operator key, clients) is in
**[DEPLOY.md](DEPLOY.md)**. The short version:

```sh
make && make phase2-all ENGINE=/path/to/colibri/c   # phase2-all optional
sudo make install                                    # or PREFIX=$HOME/.local
```

On the server, `lumabri serve --model /srv/model` opens TCP 7300 to 7302
(tracker, maintainer, executor). Add `--advertise <public-ip>` for the fastest
direct path, and `--key swarm.key` to sign the model. If a byte or compute donor
cannot accept inbound traffic, its outbound heartbeat doubles as a tracker
relay. Direct P2P remains preferred; symmetric NAT no longer excludes it from
the swarm.

On every other machine, pick a role:

| you want to | run |
|---|---|
| chat | `lumabri chat --tracker SERVER:7300 --engines-dir /path/to/colibri/c` |
| chat on the machine that holds the model | `lumabri chat --local DIR` |
| donate disk (hold bytes) | `lumabri serve --model ./slice --join SERVER:7300 --model-name NAME --donate GB` |
| donate compute (run experts) | `expert_node<engine> --model DIR --tracker SERVER:7300 --cache N` |

A disk donor is told which files to hold, rarest first, by the tracker. A
compute donor says only how many experts it can carry (`--hold N`) and the
tracker gives it the set nobody else covers. Neither needs to know the others
exist. While a reply is generating you can kill a donor: you get one failover
line and the tokens continue, identical.

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

The server also runs an expert node on the whole model, so a fresh swarm works
on day zero with the server executing everything, and donors that join later win
the calls they are nearest for. The nearest replica sets the speed: an expert
held at 2 ms and at 30 ms runs at 10.5 tok/s, not 1.4, because only your closest
copy matters.

## Tests

```sh
make test
```

runs the core suites: byte identity, donor integrity, role parsing, security
(path escape, hostile frame lengths, idle connections), protocol input
validation and prefetch policy. Per-engine expert identity runs with fixtures
(`make test-engines`), and DeepSeek V4 against a real model
(`make test-phase2-deepseek MODEL=<dir>`). Assignment, concurrency and signing
have their own scripts (`assign_test.sh`, `concurrency_test.sh`,
`sign_test.sh`). The newer mechanisms have focused targets:
`make test-cas test-key-rotation test-hedge test-relay-exec`. Every claim in
this README has a script behind it.

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

Linux, gcc, GNU make. Python 3 with numpy only for the test fixtures. A
[colibri](https://github.com/JustVugg/colibri) build provides the engine
binaries.

## Status

Working prototype, deployable. Open swarms verify bytes (sha256 per MiB and a
signed complete-model root, checked by the chatter against its own trust set)
and results (spot-check on a second replica). Private swarms add an invite token
everywhere with `LUMABRI_TOKEN`. Multi-row speculative verification, fixed-delay
hedging, a local cross-checkpoint CAS, manual old+new key rotation and NAT relay
for both READ and EXEC are implemented. Automatic SLA tuning, distributed/S3
CAS, KMS/HSM integration and automatic revocation are intentionally not part of
this dependency-free base. Expert execution is checked by replica agreement,
not by the operator signature.

## License

Apache 2.0
