# lumibri

Run huge mixture-of-experts models from a swarm of peers, with the
[colibri](https://github.com/JustVugg/colibri) engine. Pure C, no
dependencies.

One machine shares a model. Any other machine chats with it: nothing is
downloaded up front, the bytes an inference actually touches arrive from the
peer on first use and stay in a local mirror. The second question is served
from local disk at full speed. The engine binary is unmodified.

## Quick start

Build:

```sh
make
```

On the machine that has a model (any colibri model directory):

```sh
./lumibri serve --model /path/to/model
```

On the machine that wants to chat (needs a colibri build for the engine):

```sh
./lumibri chat --tracker <server-ip>:7300 --engines-dir /path/to/colibri/c
```

That is all. The first answer is slower while the working set crosses the
network; afterwards the mirror in `~/.lumibri` keeps serving even if the
server goes offline.

In the chat: `/swarm` shows the network live and anonymous (peers are
numbered, never named: model held, GB, bytes served, heartbeat), `/model`
lists the models on the swarm and switches between them, restarting the
engine on the fly. Several machines can serve different models to one
tracker with `lumibri serve --model DIR --join TRACKER:PORT`.

## How it works

`serve` runs two small programs: a tracker, which is only an index of who
holds which files, and a maintainer, which answers byte-range reads on the
model directory. A maintainer can hold just a slice of a model
(`--include` patterns), and several maintainers can share one model.

`chat` mounts the model through `liblumibri.so`, an `LD_PRELOAD` shim that
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
the internet the sequential layers are the wall, to be attacked with
speculative drafting. This build needs the small patch in
`engine_patches/olmoe-p2p.diff` applied to colibri, then:

```sh
make phase2 ENGINE=/path/to/colibri/c
./phase2_bench.sh
```

## Layout

| file | role |
|------|------|
| `lumibri.c` | the front end: `serve` and `chat` |
| `tracker.c` | index of who holds what, no model bytes |
| `maintainer.c` | serves byte ranges of a model directory |
| `lumishim.c` | the chatter-side LD_PRELOAD shim |
| `lumibri_proto.h` | binary wire protocol, header only |
| `expert_node.c` | phase 2 peer: holds experts and executes them |
| `lumibri_client.h` | phase 2 chatter side |
| `selftest.sh` | byte identity: cold, warm, offline |
| `phase2_test.sh`, `phase2_bench.sh` | phase 2 correctness and benchmark |
| `make_tiny_olmoe.py` | synthetic OLMoE-shaped fixture for tests |

## Requirements

Linux, gcc, GNU make. Python 3 with numpy only for generating the test
fixture. A [colibri](https://github.com/JustVugg/colibri) build provides the
engine binaries.

## Status

Working prototype. Not yet done, in order of importance: per-block hash
verification against a signed manifest (required before trusting unknown
peers), redundant requests against stragglers, tracker-side expert
assignment, NAT traversal. Peers are currently trusted.

## License

Apache 2.0
