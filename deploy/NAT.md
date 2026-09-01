# Donating from behind a home router (NAT)

Nothing to configure. No port forwarding, no UPnP, no VPN.

A Lumabri donor only ever makes **outbound** connections: it dials the
tracker, keeps that connection open, and the swarm's traffic to you travels
back through it (the tracker relays expert calls, byte reads and Segment
frames over the tunnel your own machine opened). A home router that allows
outbound TCP — all of them — is enough.

What changes behind NAT:

- chatters cannot dial you directly, so your answers ride the tracker relay:
  one extra hop of latency, same bytes, same verification;
- your OWN chat is the exception: a donor running next to the chat is
  recognized on loopback — resident experts are dialed at RAM distance and
  donated blocks are read at disk distance, never through the tracker;
- `lumabri doctor --tracker HOST:7300` tells you which side you are on:
  `direct-reachable` (others dial you straight) or `relay-only` (the tunnel
  carries you). Both are full members of the swarm.

## The two ways to donate

| you give | command | what happens |
|---|---|---|
| RAM + CPU | `lumabri serve --model ./donated --model-name NAME --join TRACKER:7300` | the tracker assigns a slice of experts; they load into RAM **before** the node is advertised, then execute for the swarm |
| disk | the same, plus `--donate GB` | the rarest signed blocks land on your disk and serve chatters |

Or as a system service that survives reboots: see `lumabri-donor.service`
in this directory.

## If you control every machine: a mesh VPN is a valid turbo

Lumabri needs nothing beyond outbound TCP. But when the same person owns
all the machines (a home lab, a small team), putting them in one WireGuard
or Tailscale network makes every peer directly dialable at its mesh
address — the tracker relay stops carrying any bytes and each pair of
machines talks at its own bandwidth. Point `--advertise` and `--tracker`
at the mesh IPs and everything else is unchanged. For strangers on the
open swarm, the relay remains the zero-configuration path.

## What to expect in the logs

- `auto-hold resident: up to N experts (~X GB)` — the tracker's assignment,
  sized to your free RAM;
- `holding N experts (X MB resident) loaded in Ys` — the slice is warm; only
  now does the first heartbeat go out;
- `N exec calls · M cold loads · Z% RAM hit` — you are serving traffic.

If the exec-call counter stays at zero while chats are running, the swarm's
chatters are reaching a nearer replica first — you are the failover. That
changes as more chats arrive: load spreads across every near-equal replica
by default.
