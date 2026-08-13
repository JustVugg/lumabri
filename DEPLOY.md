<img src="logo.svg" alt="lumabri" width="380">

# Running a lumabri swarm on a Hetzner server

Start to finish: a server that holds a model and executes its experts, and
clients anywhere that chat with it or donate to it. Roughly 30 minutes,
most of it waiting for the model to upload.

Everything below assumes Ubuntu 24.04 on Hetzner Cloud, but nothing is
Hetzner-specific except the firewall section.

---

## 0. What to rent

The server's job is to hold the model on disk and execute experts. It does
**not** need a GPU.

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

Open only what the swarm needs. Ports: **7300** tracker, **7301**
maintainer (byte serving), **7302** expert executor.

```sh
ufw allow OpenSSH
ufw allow 7300:7302/tcp
ufw --force enable
```

In the Hetzner Cloud console, apply the same rule in the **Firewall**
section (inbound TCP 22, 7300-7302) — the cloud firewall sits in front of
the machine and a ufw rule alone will not open it.

---

## 2. Build and install

```sh
su - lumabri
git clone https://github.com/JustVugg/lumabri.git
cd lumabri
make
exit                        # back to root for the install step
cd /home/lumabri/lumabri && make install     # → /usr/local
```

To also execute experts (phase 2) the server needs the colibri engine
source and the patch for the engine your model uses — they are per engine,
because the engines do not share a shape:

| model | patch | build | node |
|---|---|---|---|
| OLMoE | `olmoe-p2p.diff` | `make phase2` | `expert_node` |
| GLM | `colibri-p2p.diff` | `make phase2-glm` | `expert_node_glm` |
| Inkling | `inkling-p2p.diff` | `make expert_node_inkling` | `expert_node_inkling` |
| Kimi K3 | `kimi_k3-p2p.diff` | `make expert_node_kimi` | `expert_node_kimi` |
| DeepSeek V4 | `deepseek-p2p.diff` | `make expert_node_deepseek` | `expert_node_deepseek` |

DeepSeek is the one that needs a step first: colibri's `deepseek.c` is
**generated** by `tools/amalgamate_deepseek.py` and is not in a fresh clone.
`make phase2-all` notices it is missing, builds the other four and says so;
`deepseek_v4.c` is not a substitute — it is one unit of a multi-file build,
not the amalgamated one this expects.

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

Clients can preseed a strict pin file with that value for the three server
addresses:

```text
YOUR_SERVER_IP:7300 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7301 64_HEX_ENDPOINT_KEY
YOUR_SERVER_IP:7302 64_HEX_ENDPOINT_KEY
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

**`--advertise` is what gives the swarm its fastest direct path.** The
peers register with the tracker under the address they are told to publish,
and without it they publish `127.0.0.1` — which is correct for this machine
and useless for everyone. The tracker cannot fix it either: it rewrites a
localhost registration only when it arrives from off-machine, and these
arrive over loopback. A remote chatter then gets `127.0.0.1`, cannot connect,
and falls back to the outbound heartbeat tunnel for both bytes and expert
execution. That is correct behind symmetric NAT, but it adds a second network
leg and tracker load. `serve` still warns because direct P2P should be used
whenever an inbound address is available.

You should see, in order: the maintainer announcing how much it holds, the
line `ORIGIN: signed the truth of N files with <pubkey>`, the executor
`serving EXEC on :7302 ... registered with tracker`, and the tracker
accepting both under `YOUR_SERVER_IP`, not `127.0.0.1`.

`--exec-cache 256` is the RAM/SSD trade: 256 expert slots resident, the
rest streamed from disk on demand. Raise it if the box has spare RAM (the
log prints the hit rate), lower it if it swaps.

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
joins), P+1 (maintainer) and P+2 (expert executor), so open those in the
firewall for every model you add. Clients see them all with `/model`, and
switching works across architectures because the engine binary is chosen
from each model's `model_type`.

---

## 6. Clients

Every client needs the binaries (`make && sudo make install`, or just
`make` and run from the directory) and, for chatting, a colibri build for
the engine.

On first launch the chat asks what you are bringing — Enter for chat only,
2 to also donate disk, 3 (with `--model-dir`) to also execute experts. The
donors live as long as the chat does. `--role chat|disk|compute|all` skips
the question for scripts and services.

**Chat** — this is the whole point:

```sh
LUMABRI_ENCRYPT=1 \
LUMABRI_PEER_PINS=/path/to/peer-pins \
LUMABRI_PUBKEY=<contents of swarm.pub> \
lumabri chat --tracker YOUR_SERVER_IP:7300 --engines-dir ~/colibri/c
```

The public key is not optional in spirit: with it, the client verifies
every block against the operator's signature and refuses anything
unsigned. Without it the client still works, but it is trusting the server.

The first answer is slow while the dense weights cross the network; after
that `~/.lumabri` serves them locally and the swarm is only touched for
experts. `/swarm` shows the network, `/model` switches model.

Verified chunks are shared across models in `~/.lumabri/cas`; override it
with `LUMABRI_CAS=/fast/local/path`. To enable the basic straggler hedge, set
for example `LUMABRI_HEDGE_MS=40`. Keep it disabled (`0`, the default) until
there are at least two replicas per expert, otherwise there is nowhere to
hedge. Prefill and speculative target verification are batched automatically.

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

**Donate compute** — execute experts, the part that actually makes it fast.
The binary is per engine (§2 has the table); `expert_node` is OLMoE's,
`expert_node_glm` is GLM's, and so on:

```sh
expert_node_glm --model /path/to/model --tracker YOUR_SERVER_IP:7300 \
                --cache 128 --bits 8 --name my-node
```

`--bits` must match what the chatter's engine uses (its argv[2], default 8):
for a model without pre-quantized tensors the loader quantizes on the way in,
and a peer at different bits holds different weights. DeepSeek V4 has no such
knob — and its peer is the cheap one to run, since the V4 expert store needs
no dense weights at all.

Chatters discover it automatically and route to it when it is nearer than
the server. Kill it and they fail over — ultimately back to the server,
which holds everything.

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

Two things worth knowing:

- **A tracker restart rebuilds placements automatically.** Liveness is in RAM
  and every peer re-registers within one heartbeat (10 s). Name-to-peer-key
  ownership is persisted under `~/.lumabri/tracker_peer_bindings`, so restart
  does not reopen names for takeover.
- **A client with a warm mirror keeps working with the server off.** That
  is the design, and `selftest.sh` pass 3 tests exactly it.

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
| `no swarm at HOST:7300` | cloud firewall: open 7300-7302 in the Hetzner console too |
| the server logs nothing for minutes on first start | it is hashing the model; the progress lines say how far along |
| `il motore non è arrivato a essere pronto` | the engine's own last 25 lines are printed underneath — read those |
| engine killed by signal 9 | out of memory: lower `--ctx` and `--cap`, or take a bigger box |
| the chatter fills the disk | it mirrors what it reads; on the server use `--local DIR` instead |
| client says `not signed by the operator key` | server not started with `--key`, or a different key than the client's `LUMABRI_PUBKEY` |
| `engine not found` on the client | pass `--engines-dir /path/to/colibri/c` |
| tracker logs `REJECTED: ... unsigned` | a peer joined a signed swarm without signed truth — that is the defence working |
| expert cache hit rate very low | raise `--exec-cache`; each slot is one expert of RAM |
| first start takes minutes | hashing the model; cached afterwards in `.lumabri_hashes/` |
