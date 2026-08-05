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
source and the small patch:

```sh
su - lumabri
git clone https://github.com/JustVugg/colibri.git
cd colibri && git apply ~/lumabri/engine_patches/olmoe-p2p.diff
cd ~/lumabri && make phase2 ENGINE=$HOME/colibri/c
exit
cd /home/lumabri/lumabri && make install
```

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
caches the result in `.lumabri_hashes/`, so only that first start pays.

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
    --key /home/lumabri/swarm.key \
    --exec-cache 256
Restart=always
RestartSec=5
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

You should see, in order: the maintainer announcing how much it holds, the
line `ORIGIN: signed the truth of N files with <pubkey>`, and the executor
`serving EXEC on :7302 ... registered with tracker`.

`--exec-cache 256` is the RAM/SSD trade: 256 expert slots resident, the
rest streamed from disk on demand. Raise it if the box has spare RAM (the
log prints the hit rate), lower it if it swaps.

---

## 6. Clients

Every client needs the binaries (`make && sudo make install`, or just
`make` and run from the directory) and, for chatting, a colibri build for
the engine.

**Chat** — this is the whole point:

```sh
LUMABRI_PUBKEY=<contents of swarm.pub> \
lumabri chat --tracker YOUR_SERVER_IP:7300 --engines-dir ~/colibri/c
```

The public key is not optional in spirit: with it, the client verifies
every block against the operator's signature and refuses anything
unsigned. Without it the client still works, but it is trusting the server.

The first answer is slow while the dense weights cross the network; after
that `~/.lumabri` serves them locally and the swarm is only touched for
experts. `/swarm` shows the network, `/model` switches model.

**Donate disk** — hold part of the model so the swarm survives your server:

```sh
lumabri serve --model ./slice --join YOUR_SERVER_IP:7300 \
              --model-name mymodel --donate 20
```

The tracker assigns the 20 GB where the swarm is thinnest (rarest first),
the donor pulls it — verifying every byte against the signed truth — and
then serves it.

**Donate compute** — execute experts, the part that actually makes it fast:

```sh
expert_node --model /path/to/model --tracker YOUR_SERVER_IP:7300 \
            --cache 128 --name my-node
```

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

- **A tracker restart is free.** State is in RAM and every peer
  re-registers within one heartbeat (10 s).
- **A client with a warm mirror keeps working with the server off.** That
  is the design, and `selftest.sh` pass 3 tests exactly it.

---

## 8. Hardening, briefly

- SSH keys only: `PasswordAuthentication no` in `/etc/ssh/sshd_config`.
- Private swarm: set `LUMABRI_TOKEN` in the unit **and** on every client.
  It is checked by the tracker, every maintainer and every expert node.
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
| client says `not signed by the operator key` | server not started with `--key`, or a different key than the client's `LUMABRI_PUBKEY` |
| `engine not found` on the client | pass `--engines-dir /path/to/colibri/c` |
| tracker logs `REJECTED: ... unsigned` | a peer joined a signed swarm without signed truth — that is the defence working |
| expert cache hit rate very low | raise `--exec-cache`; each slot is one expert of RAM |
| first start takes minutes | hashing the model; cached afterwards in `.lumabri_hashes/` |
