# Lumabri — the home cluster

Turn the computers you already have into one machine that runs a model none
of them could hold alone.

## Current implementation boundary

The home-cluster roadmap is not complete yet. The current code has the exact
eight-adapter registry, a loopback Segment proof, a conservative memory
preflight, a local catalogue/TUI, and one-session Hosted chat by explicit
address. The catalogue does not yet collect a distributed LAN inventory or
apply its proposed plan. Adapter-specific memory sizing is currently verified
only for OLMoE and DeepSeek V4; the other registered adapters are shown as
experimental/unsized until their C planner and real-checkpoint conformance run
land. A real two-physical-machine LAN gate is still required.

```
lumabri models          # current local planning preview
lumabri host --model M  # this machine owns the model and serves sessions
lumabri chat --host H   # this machine holds nothing and just types
```

## What it actually does

A model is split into contiguous ranges of layers, one range per computer.
Each node keeps the state for its own layers. Activations cross the LAN
between them; the weights never move again once they are in place.

That is where the gain is, and it is worth being exact about which gain.
**Adding computers always adds memory, capacity and availability. It makes a
single chat faster only when the new plan removes disk reads or brings faster
hardware.** Layers are sequential: twelve layers on one machine cost about
what six plus six cost on two, plus a hop. What changes everything is the
other case — a model that one machine would stream from NVMe and three hold
in RAM.

## The three states

`lumabri models` puts every checkpoint in exactly one of them:

| | meaning |
|---|---|
| **resident** | every weight of every range is in RAM or VRAM |
| **from disk** | the working set is resident and the rest is read from NVMe or CAS — only where an adapter has demonstrated it |
| **not runnable** | not even the working set fits, or no adapter matches, or the data is not reachable |

The working set is the floor: dense weights, an expert cache at least as
large as top-k, kernel scratch, state. Below it a node cannot run at all,
whatever the disk can stream.

When something does not fit, the shortfall is reported in gigabytes **and in
machines** — gigabytes are not something a person can go and buy.

## The speed column says "not calibrated"

And it keeps saying that until a calibration has been run on **this** cluster
with **this** plan. Download and preparation times are estimated, because
they come from known byte counts and measured bandwidth. Speed is not
estimated, ever. A measurement stops being valid when any of these changes:

```
model_root · adapter + ABI · numeric class · commit · build id
nodes and hardware · backend · assigned ranges · resident or disk
threads · context · sessions
```

Change one and the number is stale, and says so rather than being reused.

## Hosted chat

`lumabri chat --host HOST:PORT` downloads **no part of the model**. Not the
weights, not a mirror, not even a tokenizer — the engine tokenizes the prompt
on the host side. A test counts the bytes and fails if any appear.

The screen says which host is answering, whether its speed has been measured,
and that **the host runs the model and therefore receives the text of the
conversation**. That sentence belongs before the first prompt, not in a
document.

A host serves one session at a time and answers **BUSY** to the second. Not a
queue: a queue nobody can see is how "it got slow" replaces "it is full".
Between two clients the engine restarts, so no one continues someone else's
conversation.

## Which models

Every checkpoint whose family has a Colibri Edge/Segment adapter. A new
adapter is discovered automatically, but becomes *supported* only once it has
an exact mapping, a planner and a conformance run — until then the catalogue
calls it experimental, and CI fails while the two registries disagree.

## Verifying it yourself

```
bash ./segment_split_test.sh    # loopback: same tokens on one node and two
bash ./segment_budget_test.sh   # a range that does not fit is refused, and says why
bash ./multi_session_test.sh    # contention may make a session wait, never change it
bash ./hosted_chat_test.sh      # zero checkpoint bytes on the client
bash ./catalog_test.sh          # no invented speeds, shortfall in machines
bash ./adapter_conformance_test.sh
```

`segment_split_test.sh` is a loopback rehearsal, not a LAN benchmark. It
withholds its timings when the machine's load average
exceeds its core count, because on a busy box the numbers describe the
contention rather than the topology. The token comparison still runs: that
one is machine-independent. The two-physical-machine LAN gate remains a
separate roadmap item.
