<img src="logo.svg" alt="lumabri" width="380">

# Running a lumabri swarm on a Hetzner server

Start to finish: a server that holds a model and supplies complete inference, and
clients anywhere that chat with it or donate to it. Roughly 30 minutes,
most of it waiting for the model to upload.

Everything below assumes Ubuntu 24.04 on Hetzner Cloud, but nothing is
Hetzner-specific except the firewall section.

Before starting a host, run `lumabri doctor --tracker HOST:7300 --model
/srv/model --serve-port 7300` (or add `--json` for automation). It distinguishes
required failures from resource and optional-Segment warnings. The complete
release evidence and soak procedure are in [PRODUCTION.md](PRODUCTION.md).

---

## 0. What to rent

The server's job is to hold the model on disk and bootstrap complete Segment
coverage plus the classic expert fallback. It does **not** need a GPU.

| you serve | disk | RAM | reasonable Hetzner box |
|---|---|---|---|
| a small MoE (OLMoE-class, ~14 GB) | 40 GB+ | 8 GB | CPX21 / CAX21 |
| a mid MoE (~80 GB) | 160 GB+ | 16 GB | CPX41 + volume |
| a large MoE (200 GB+) | volume | 16-32 GB | CPX41 + 500 GB volume |

Disk matters more than RAM: with `--exec-cache` the experts stream from the
SSD and only the hot ones sit in memory. **Hetzner Cloud volumes are network
storage** — fine for phase-1 byte serving, but for expert *execution* keep
the model on the local NVMe of the instance if you can, since a cold expert
becomes a network round trip inside the datacentre otherwise.

Traffic: Hetzner Cloud includes 20 TB/month on most plans. A cold chatter
downloads the dense part once (a few GB) and then nothing, so this is
usually a non-issue — but it is the number to watch if the swarm grows.

---

## 1. Server: user, packages, firewall

SSH in as root, then:

```sh
apt update && apt install -y build-essential git python3-numpy ufw
adduser --disabled-password --gecos "" lumabri
```

Open only what the swarm needs. **7300** is the required tracker/control port.
The signed relay makes every data path work with only that inbound port. For
the lower-latency direct path also open **7301** (bytes), **7302** (classic
experts) and, by default, **7303** (one full-core Segment fallback). Allow through
7309 only when intentionally using up to seven public ranges.

```sh
ufw allow OpenSSH
ufw allow 7300/tcp
# fastest public-server path (optional): ufw allow 7301:7309/tcp
ufw --force enable
```

In the Hetzner Cloud console, apply the same selected rule in the **Firewall**
section — the cloud firewall sits in front of the machine and a ufw rule alone
will not open it.

---

## 2. Build and install

```sh
su - lumabri
git clone https://github.com/JustVugg/lumabri.git
cd lumabri
git clone https://github.com/JustVugg/colibri.git ../colibri
make ENGINE=$HOME/colibri/c
exit                        # back to root for the install step
cd /home/lumabri/lumabri && make install     # → /usr/local
```

The command above builds automatic Segment execution for every adapter in the
matching Colibri dev checkout. To retain the classic fine-grained expert
fallback too, build the patch for the engine your model uses — they are per engine,
because the engines do not share a shape:

| model | patch | build | node |
|---|---|---|---|
| OLMoE | `olmoe-p2p.diff` | `make phase2` | `expert_node` |
| GLM | `colibri-p2p.diff` | `make phase2-glm` | `expert_node_glm` |
| Inkling | `inkling-p2p.diff` | `make expert_node_inkling` | `expert_node_inkling` |
| Kimi K3 | `kimi_k3-p2p.diff` | `make expert_node_kimi` | `expert_node_kimi` |
| DeepSeek V4 | `deepseek-p2p.diff` | `make expert_node_deepseek` | `expert_node_deepseek` |
| Qwen3.6 | `qwen36-p2p.diff` | `make expert_node_qwen36` | `expert_node_qwen36` |

DeepSeek is built from whichever layout your colibri checkout ships. Current
colibri has the unit-amalgamated `deepseek_v4.c`; `make expert_node_deepseek`
compiles its `COLI_V4_UNIT_*` objects and links them, no extra step. Older
colibri trees had a single generated `deepseek.c`, which still works. On the
current multi-file layout **both** sides build: the **expert node** (the donor)
and the **chatter** (chat-side expert delegation). The chatter is the newer
half — `deepseek_v4.c` is unit-amalgamated, so instead of the old line patch it
is hooked by an anchored script and a single client bridge object. `make
chatters` includes DeepSeek and prints no "skipped" line, so a DeepSeek swarm
both serves/executes experts and delegates them from the chat side. Verified
byte-identical against a real V4 model: 3087 remote expert calls over 215 layer
rounds, PREFILL 10/10 and GREEDY 4/4 against the local oracle.

Skip this whole section if you only want phase 1: a server that serves the
model's bytes works for every engine, DeepSeek included, and `lumabri chat`
speaks every engine's protocol. Phase 2 is what stops the chatter from
downloading expert weights.

```sh
su - lumabri
git clone https://github.com/JustVugg/colibri.git
cd ~/lumabri && make phase2-all ENGINE=$HOME/colibri/c
exit
cd /home/lumabri/lumabri && make install
```

`make phase2-all` builds both halves for every engine and **does not touch
the colibri checkout**: each patch is applied to a copy under `build/`.

Both halves matter and they are easy to confuse:

| you want to | you need | binary |
|---|---|---|
| chat with experts on peers | the patched engine | `colibri_p2p`, `olmoe_p2p`, … |
| donate compute | the expert node | `expert_node_glm`, `expert_node`, … |

`make chatters` builds the first set, `make engines` the second. Without the
patched engine a chatter still works — but phase 2 never engages and it
quietly downloads expert weights instead of asking peers to run them. The
tell is the missing `phase 2 active` line at boot.

All five paths are proven byte-identical: `make test-engines` runs the four
that have synthetic fixtures, and `make test-phase2-deepseek MODEL=<dir>`
runs V4 against a real model.

The DeepSeek peer is worth knowing about even if you serve something else:
it holds experts and no dense weights at all, so a donor for a 156 GB V4
model needs a few hundred MB of RAM and the disk.

Sanity check before going further — it runs in seconds and proves the whole
byte path is byte-identical:

```sh
su - lumabri -c "cd ~/lumabri && ./selftest.sh"
```

---

## 3. The operator key

This is what makes the swarm trustworthy: the key signs which bytes are
real, so nobody — not even a compromised tracker — can serve you a
different model.

```sh
su - lumabri
cd ~
lumabri key --out swarm
```

You get `swarm.key` (secret, mode 0600) and `swarm.pub` (32 hex bytes).

**Copy `swarm.key` somewhere safe offline and consider deleting it from the
server once the model is signed.** The signatures are computed at startup
from the hashes, so the key is only needed when the model changes. If you
keep it on the server for convenience, that is a deliberate trade: an
attacker with root there could then re-sign altered weights.

The public value is what you hand to every user. It is not a secret — post
it, put it in the README of your swarm, read it over the phone:

```sh
cat swarm.pub
```

This signing key authenticates model contents. Transport endpoints use a
separate per-machine identity. Print the server's public endpoint key as the
service user:

```sh
su - lumabri -c 'lumabri peer-key'
```

Clients can preseed a strict pin file with that value for every server
endpoint (the default Segment fallback uses port 7303; an explicit
`LUMABRI_SEGMENT_CHUNKS=N` uses 7303 through `7302+N`):

```text
YOUR_SERVER_IP:7300 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7301 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7302 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7303 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7304 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7305 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7306 64_HEX_ENDPOINT_KEY
```

Without a strict file, encrypted clients persist first contact in
`~/.lumabri/known_hosts` and refuse later key changes. That is convenient but
does not protect the very first connection from an active MITM. A strict pin
file does. New direct donor endpoints must be added to the file; unlisted NAT
donors remain reachable through the pinned tracker's relay.

Endpoint-key rotation uses an overlap just like host-key rotation: add a second
row for the same address with the new endpoint key, switch the server, then
remove the old row. A TOFU `known_hosts` file deliberately accepts one key per
address and must be edited only after the new key is verified out of band.

### Manual key rotation

Create the replacement key, then distribute a trust file containing the old
and new public keys, one per line:

```sh
lumabri key --out swarm-next
cat swarm.pub swarm-next.pub > swarm-trust.pub
```

Point tracker/maintainers at `--pubkey swarm-trust.pub` and clients at
`LUMABRI_PUBKEY=/path/to/swarm-trust.pub` before signing anything with
`swarm-next.key`. Once every verifier has the overlap trust file, switch the
origin to the new secret and restart it. After the new signatures have been
observed and old clients are gone, remove the old line. A comma-separated pair
also works. Keep the newest key last: valid signatures from later keys replace
earlier ones at the tracker, never the reverse. Restart long-running verifiers
after changing the trust file. This is manual rotation and manual revocation; KMS/HSM policy and
audit are outside the dependency-free core.

---

## 4. The model

Put it under `/home/lumabri/models/<name>` — a normal colibri model
directory (`config.json`, tokenizer, `*.safetensors`).

```sh
# from your own machine
rsync -avP --info=progress2 /path/to/model/ \
      lumabri@YOUR_SERVER_IP:/home/lumabri/models/mymodel/
```

First start hashes the whole model (sha256 per MiB, ~1-2 GB/s per core) and
caches the result in `.lumabri_hashes/`, so only that first start pays. It
tells you how it is going — for a 300 GB model expect several minutes with
nothing served until it finishes:

```
[maintainer peer-7301] integrity: hashing 148 files, 299.0 GB. Only the first
                       start pays this — cached in .../models/glm/.lumabri_hashes.
[maintainer peer-7301] hashing 42.1/299.0 GB (14%) · 780 MB/s · ~5 min left · model-00012-of-00148.safetensors
```

---

## 5. Run it as a service

`/etc/systemd/system/lumabri.service`:

```ini
[Unit]
Description=lumabri swarm server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=lumabri
WorkingDirectory=/home/lumabri
ExecStart=/usr/local/bin/lumabri serve \
    --model /home/lumabri/models/mymodel \
    --port 7300 \
    --advertise YOUR_SERVER_IP \
    --key /home/lumabri/swarm.key \
    --exec-cache 256
Restart=always
RestartSec=5
Environment=LUMABRI_ENCRYPT=1
# a private swarm: uncomment and set the same value on every client
# Environment=LUMABRI_TOKEN=choose-a-long-random-string

[Install]
WantedBy=multi-user.target
```

```sh
systemctl daemon-reload
systemctl enable --now lumabri
journalctl -u lumabri -f
```

**A reachable address gives the swarm its fastest path.** `serve` detects a
public IPv4 attached to the machine and publishes it automatically. If the
host sits behind NAT, pass `--advertise` only after forwarding the advertised
ports. Otherwise Lumabri publishes Segment as relay-only and carries bytes,
experts and stateful layer runs through signed outbound tracker tunnels.

You should see, in order: the maintainer announcing how much it holds, the
line `ORIGIN: signed the truth of N files with <pubkey>`, the classic executor
and the Segment origin ranges. A public deployment should advertise direct
endpoints under `YOUR_SERVER_IP`; a NAT deployment explicitly reports
`data plane relay` and needs no public Segment endpoint.

`--exec-cache 256` is the RAM/SSD trade: 256 experts resident in the whole
executor, the rest streamed from disk on demand. Raise it if the box has
spare RAM (the log prints the hit rate and, for DeepSeek, the resulting GB),
lower it if it swaps. Size it from the expert, not the slot count: a
DeepSeek V4 Flash expert is ~15 MB with headroom, so `--exec-cache 1800` is
about 27 GB, and the store keeps at least top-k experts per layer (258 on
V4-Flash) whatever you pass. `serve` subtracts this figure from the RAM it
offers the Segment origin slices, and a slice whose layer range does not fit
its share resident is not started (exit code 3, not restarted): on a box
smaller than the model you get storage plus the streaming executor, which is
the honest capacity, instead of five caches of the same experts fighting for
one RAM.

---

## 5b. More than one model on the same server

The tracker is an index, not a model server: one of them holds as many models
as you give it. Add a unit per model, all joining the first one's tracker:

```ini
ExecStart=/usr/local/bin/lumabri serve \
    --model /home/lumabri/models/second \
    --join 127.0.0.1:7300 \
    --port 7310 \
    --key /home/lumabri/swarm.key \
    --exec-cache 256
```

Ports go up in tens: each `serve` uses P (its own tracker, unused when it
joins), P+1 (maintainer), P+2 (expert executor) and normally P+3 through P+6
(Segment ranges), so open that block for every model you add. Clients see them all with `/model`, and
switching works across architectures because the engine binary is chosen
from each model's `model_type`.

---

## 6. Clients

Every client needs Lumabri built against the matching Colibri Segment ABI
(`make ENGINE=/path/to/colibri/c && sudo make install`). Keep the ordinary or
P2P Colibri executables installed too if you want the classic fallback when a
complete Segment route is unavailable.

On first launch the chat asks what you are bringing — Enter for chat only,
2 to also donate disk, 3 to donate compute automatically. The
donors live as long as the chat does. `--role chat|disk|compute|all` skips
the question for scripts and services.

Only one automatic compute role is admitted per machine/user across both chat
and `serve --join`. If another process already owns the donable RAM, Lumabri
prints its PID/model/tracker and leaves the second process storage-only. This is
intentional: one resident executor receives the complete disjoint assignment;
four independently auto-sized executors would all reserve the same RAM. Stop
old Lumabri parents before retesting a new build. On Linux, executors spawned by
the new build terminate automatically when their parent dies.

**Chat** — this is the whole point:

```sh
LUMABRI_ENCRYPT=1 \
LUMABRI_PEER_PINS=/path/to/peer-pins \
LUMABRI_PUBKEY=<contents of swarm.pub> \
lumabri chat --tracker YOUR_SERVER_IP:7300
```

`--engines-dir ~/colibri/c` is optional when Lumabri finds the installation;
it is still a useful explicit fallback path during development.

The public key is not optional in spirit: with it, the client verifies
every block against the operator's signature and refuses anything
unsigned. Without it the client still works, but it is trusting the server.

With Segment, the first answer fetches only the local Edge working set
(tokenizer, embedding, final transform/head) into `~/.lumabri`; layer weights
and state remain on peers. If Segment is unavailable, the same TUI starts the
classic expert/CAS path and its dense working set automatically. `/swarm` and
`/hosts` show named machines and their live roles, `/experts` shows executor
call counts and Segment activity, and `/model` switches model. Slash commands
autocomplete with Tab. During generation the fixed input dock remains usable:
status shows routing/prefill/decode/failover and read-only menus open without
waiting for the answer to finish.

Verified chunks are shared across models in `~/.lumabri/cas`; override it
with `LUMABRI_CAS=/fast/local/path`. Expert routing learns an EWMA and slow-tail
estimate per replica. It hedges automatically only after at least eight samples
show a material p95 tail and another replica exists; set
`LUMABRI_HEDGE_MS=40` to force a fixed policy, or leave the variable unset for
the adaptive policy. Three consecutive failures open a short circuit instead
of permanently deleting a valid manifest. Prefill and speculative target
verification are batched automatically.

Segment compute has a FIFO admission queue (`LUMABRI_SEGMENT_RUN_QUEUE`, default
32) and a real deadline (`LUMABRI_SEGMENT_RUN_WAIT_MS`, default 30000). Queue and
inflight counts are advertised to new placements. Keep the queue bounded: it is
backpressure, not extra capacity. Current Colibri adapters serialize calls to
one engine instance for numeric/state safety; aggregate concurrency comes from
the layer pipeline and compatible replicas, while Expert executors continue to
run their safe per-machine parallel gate.

The boot narrates itself, so you can see where the time goes:

```
  chiedo allo sciame chi ha glm…
  148 file · 299 GB · 1 peer · tipo glm4_moe
  mirror in /home/you/.lumabri: 512 GB liberi. Tiene solo i blocchi che tocchi…
  motore /home/you/colibri/c/colibri
  ora scarico la parte densa una volta sola — gli esperti restano sullo sciame
  ✻ net 6.2 MB in 12 blocks… · 4.1/299 GB · 92 MB/s · 47s
```

**On the machine that already holds the model** — the server itself — do not
chat through the swarm: the mirror would be a second full copy of the model
on the same disk. Read it where it is:

```sh
lumabri chat --local /home/lumabri/models/mymodel --engines-dir ~/colibri/c
```

Useful knobs when RAM is tight: `--ctx` (context tokens, the KV pool scales
with it) and `--cap` (resident experts, default 64).

**Donate disk** — hold part of the model so the swarm survives your server:

```sh
lumabri serve --model ./slice --join YOUR_SERVER_IP:7300 \
              --model-name mymodel --donate 20
```

The tracker assigns the 20 GB where the swarm is thinnest (rarest first),
the donor pulls it — verifying every byte against the signed truth — and
then serves it.

**Donate compute** — choose `compute` in the normal TUI. If the current chat
uses Segment and a range fits after the RAM reserve, the tracker assigns its
rarest origin range automatically. A reachable machine uses direct P2P; a NAT
machine uses Segment relay. If the range does not fit, Lumabri starts the
finer-grained expert donor. Manual expert commands remain useful diagnostics:

```sh
expert_node_glm --model /path/to/model --tracker YOUR_SERVER_IP:7300 \
                --cache 128 --bits 8 --name my-node
```

`--bits` must match what the chatter's engine uses (its argv[2], default 8):
for a model without pre-quantized tensors the loader quantizes on the way in,
and a peer at different bits holds different weights. DeepSeek V4 has no such
knob — and its peer is the cheap one to run, since the V4 expert store needs
no dense weights at all.

Chatters discover it automatically. Ordinary Segment ranges replace matching
fallback origin ranges on the next route generation, so the original server
does progressively less work while retaining full coverage. Expert requests
fail over to replicas. Stateful Segment checkpoints at completed turns when a
compatible replica exists; a dead selected peer is replaced by an exact-range
replica, restored and replayed. If no compatible replica exists the run fails
explicitly.

---

## 7. Checks and operations

```sh
# is the swarm alive and who is on it
lumabri chat --tracker 127.0.0.1:7300   # then type /swarm

# server logs
journalctl -u lumabri -f

# after changing the model files: drop the cached hashes so they are
# recomputed and re-signed
rm -rf /home/lumabri/models/mymodel/.lumabri_hashes
systemctl restart lumabri
```

Use `--host-name NAME` on `lumabri serve` when the automatic hostname is not
recognizable. The storage, expert and Segment children inherit that prefix,
and the server prints a periodic aggregate whenever hosts, calls or active
sessions change.

The Segment relay is deliberately bounded even though its end-user `LMB_RSEG`
request does not yet contain a caller signature. The tracker applies a token
bucket and concurrency cap per observed source, and waits only a bounded time
for the serialized executor tunnel. Defaults and overrides are:

```sh
LUMABRI_RSEG_RATE=2048                 # weighted requests/second/source
LUMABRI_RSEG_BURST=4096
LUMABRI_RSEG_SOURCE_CONCURRENCY=32
LUMABRI_RSEG_QUEUE_MS=2000
```

Private swarms should still set `LUMABRI_TOKEN`; source rate limiting is abuse
containment, not a cryptographic end-user identity. Direct Segment remains the
preferred data plane.

Resource defaults for automatic origin slices are conservative:

```sh
LUMABRI_SEGMENT_MIN_FREE_MB=8192       # do not auto-start below this
LUMABRI_SEGMENT_RAM_RESERVE_MB=4096    # reject new sessions below this
LUMABRI_SEGMENT_SESSIONS=2             # optional per-slice override (1..64)
```

Threads are divided across layer slices, only one OpenMP team runs per slice,
and fallback/NAT executors are niced. Direct origins default to four sessions
per slice and NAT/relay origins to two; the override above is bounded but can
increase real KV RAM substantially. These ranges consume real RAM and CPU; use
`--no-exec` when this server must provide storage only.

`serve` gives every Segment child an explicit process budget computed as
`(MemAvailable - LUMABRI_SEGMENT_MIN_FREE_MB) / slices`. The node forwards it
to Colibri's engine options, divides the remaining post-engine RSS among its
session slots, and cancels a run when RSS or the system reserve is crossed. A
compute donor receives the total layer count with its tracker assignment and
evaluates the assigned range—not a fixed quarter of the model. If it does not
fit, the placement promise is released immediately and the chat starts the
finer-grained Expert donor instead.

Low-level operators may set `segment_node --memory-limit-mb N`; the ordinary
`lumabri serve` and `lumabri chat` flows calculate it automatically.

Two things worth knowing:

- **A tracker restart rebuilds placements automatically.** Liveness is in RAM
  and every peer re-registers within one heartbeat (10 s). Name-to-peer-key
  ownership is persisted under `~/.lumabri/tracker_peer_bindings`, so restart
  does not reopen names for takeover.
- **A client with a warm mirror keeps working with the server off.** That
  is the design, and `selftest.sh` pass 3 tests exactly it.
- **`.coli_*` files never leave their machine.** They are mutable Colibri
  runtime telemetry/cache state, not signed model bytes. If a tracker still
  prints `REJECTED ... unsigned .../.coli_usage`, restart that donor with the
  same current Lumabri build; an older maintainer process is still announcing
  its startup manifest.

---

## 8. Hardening, briefly

- SSH keys only: `PasswordAuthentication no` in `/etc/ssh/sshd_config`.
- Private swarm: set `LUMABRI_TOKEN` in the unit **and** on every client.
  It is checked by the tracker, every maintainer and every expert node. Keep
  `LUMABRI_ENCRYPT=1`; a bearer token over plaintext is not private.
- For production, distribute `LUMABRI_PEER_PINS`. Persistent TOFU detects
  later key replacement, while pins also authenticate first contact.
- Keep the operator secret key off the server if you can (see §3).
- The peers you accept are still untrusted-but-verified: they cannot change
  bytes (signatures) and cannot fake results undetected if clients run
  `LUMABRI_VERIFY=5` or higher. They *can* refuse to answer — that costs a
  failover, not correctness.

---

## Troubleshooting

| symptom | cause |
|---|---|
| `no swarm at HOST:7300` | cloud firewall: open tracker TCP 7300 in the provider console too |
| the server logs nothing for minutes on first start | it is hashing the model; the progress lines say how far along |
| `il motore non è arrivato a essere pronto` | the engine's own last 25 lines are printed underneath — read those |
| engine killed by signal 9 | out of memory: lower `--ctx` and `--cap`, or take a bigger box |
| the chatter fills the disk | it mirrors what it reads; on the server use `--local DIR` instead |
| client says `not signed by the operator key` | server not started with `--key`, or a different key than the client's `LUMABRI_PUBKEY` |
| `engine not found` after Segment fallback | install the classic Colibri engine or pass `--engines-dir /path/to/colibri/c` |
| tracker logs `REJECTED: ... unsigned` | a peer joined a signed swarm without signed truth — that is the defence working |
| expert cache hit rate very low | raise `--exec-cache`; each slot is one expert of RAM (~15 MB on DeepSeek V4 Flash), spread over the layers |
| first start takes minutes | hashing the model; cached afterwards in `.lumabri_hashes/` |
