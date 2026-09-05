# Direct Segment execution

Lumabri uses Colibri's public Edge/Segment ABI to run one model as a
chain of layer-aligned peers:

```text
prompt -> local Edge -> peer A [0:k] -> peer B [k:n] -> local Edge -> token
```

The registry is model-neutral and mirrors all eight adapters currently exposed
by Colibri: GLM, GLM-5.3, Inkling, Kimi, OLMoE, Qwen3.6, Qwen3.8 and DeepSeek
V4. Registration is not the same as conformance: the legacy real-checkpoint
release gate covers GLM, Inkling, Kimi K3, OLMoE, Qwen3.6 and DeepSeek V4;
GLM-5.3 and Qwen3.8 remain experimental until their fixture runs pass the same
gate. Each gated family is split over two peers and its generated token IDs
are compared with an independent Colibri oracle. The gate also overlaps two OLMoE
chats against the same executors to exercise real session isolation, and
retransmits a committed run to prove the cached duplicate response is exact.

This is now the preferred data plane of the ordinary `lumabri chat` TUI. If
the runtime, identity or a complete compatible route is absent, startup falls
back automatically to the existing expert/CAS Colibri path. Colibri's ordinary
executables remain unchanged.

Each Segment executor can also use the classic Expert swarm inside its range.
Attention, recurrent state and dense blocks stay resident in the Segment;
when the tracker has complete resident coverage for one MoE layer, all of that
layer's selected experts are issued in parallel to the donor peers before any
reply is collected. Other layers remain local. A dead donor rolls back partial
accumulation and runs the unchanged local kernel, so Expert donation is an
optional accelerator rather than a new failure dependency.

This integration does not patch a Colibri checkout. Lumabri copies the engine
tree to `build/segment-hybrid-colibri`, applies exact-count hooks there and
links only `segment_node` against that private archive. A missing or changed
anchor fails the Lumabri build. Ordinary Colibri builds and binaries are never
modified.

## Build

Use a Colibri checkout containing the additive Edge runtime:

```sh
make ENGINE=/path/to/colibri/c
```

When the two additive ABI headers exist, ordinary `make` includes
`segment_node` and `segment_chat`; older Colibri release trees keep the legacy
build. `segment-edge-library` contains both public runtimes and every adapter
exposed by the selected Colibri checkout, and is not linked into ordinary
Colibri executables.

## Normal use

The model owner runs:

```sh
lumabri serve --model /models/olmoe --advertise PUBLIC_IP
```

Lumabri detects the family, layer/context shape, CPU/RAM and network capability,
then starts four stable layer-aligned fallback executors by default. Every
sequential slice receives the full CPU team; `LUMABRI_SEGMENT_CHUNKS=N`
overrides the split and `LUMABRI_SEGMENT_THREADS=N` overrides its CPU team.
A public interface enables the
preferred direct path. Behind NAT the same executors register as relay-only and
remain reachable through their signed outbound tracker tunnels; Lumabri never
publishes a guessed private address as direct.

Every client runs the unchanged TUI:

```sh
lumabri chat --tracker SERVER:7300
```

It obtains the signed aggregate model identity from the tracker, opens local
Edge through the CAS mirror, discovers Segment and expert coverage in the
background and selects the complete Segment chain when available. There are no
user-facing model roots, tokenizer roots, ranges or `segment_chat` command.

Choosing `compute` in the TUI asks the tracker for the least-replicated exact
origin range. The node loads that slice from CAS at low priority if it fits
after the machine reserve, using direct P2P or automatic NAT relay. Otherwise
Lumabri donates auto-sized experts over the relay-capable classic path.

## Low-level two-peer diagnostic

Start a tracker on the publicly reachable control server:

```sh
./tracker --port 7300 --peer-bindings ./peer-bindings
```

Split a four-layer example model over two reachable machines. Both peers must
use the same model name, model root, tokenizer root, schema and numeric class;
the latter three model properties are read from Colibri automatically.

```sh
./segment_node \
  --engine olmoe --model-dir /models/olmoe --model my-olmoe \
  --range 0:2 --port 7303 --tracker TRACKER:7300 \
  --advertise PEER_A_PUBLIC_IP:7303 --name olmoe-a \
  --model-root MODEL_HEX64 --tokenizer-root TOKENIZER_HEX64 \
  --context 4096 --max-rows 64 --sessions 4
```

```sh
./segment_node \
  --engine olmoe --model-dir /models/olmoe --model my-olmoe \
  --range 2:4 --port 7303 --tracker TRACKER:7300 \
  --advertise PEER_B_PUBLIC_IP:7303 --name olmoe-b \
  --model-root MODEL_HEX64 --tokenizer-root TOKENIZER_HEX64 \
  --context 4096 --max-rows 64 --sessions 4
```

The low-level chatter needs the model's Edge files (tokenizer, embeddings,
final transform and head), then discovers a complete compatible route:

```sh
./segment_chat \
  --engine olmoe --model-dir /models/olmoe-edge --model my-olmoe \
  --tracker TRACKER:7300 \
  --model-root MODEL_HEX64 --tokenizer-root TOKENIZER_HEX64 \
  --prompt "Hello" --tokens 32 --context 4096 --max-rows 64
```

For an isolated experiment with no byte-plane maintainer registered under the
same model name, the two roots may be any shared non-zero 32-byte hex values.
For a real swarm, prefer `--auto-identity`; explicit roots remain only for
isolated fixtures and protocol diagnosis. A donor may use `--auto-range` in
place of `--range`: the tracker keeps a short loading promise so concurrent
donors do not all choose the same rare slice.

Open the Segment port on each executor's firewall only when publishing a direct
address. Without `--advertise`, `lumabri serve` and compute donors use the exact
peer's outbound tracker connection for `OPEN`, `RUN`, `SNAPSHOT`, `RESTORE` and
`CLOSE`; tracker TCP 7300 is the only required inbound port. Direct is preferred
when both transports are available and a failed direct request is retried by
relay with the same idempotent request ID.

## Sampling and recovery

Colibri Edge ABI v2 exposes the full logits for the conforming adapters. The ordinary
TUI's temperature and top-p are therefore honored on the Segment path. A zero
temperature calls Colibri's original greedy selector unchanged; a positive
temperature uses deterministic-seedable nucleus sampling (`--seed` or
`LUMABRI_SAMPLE_SEED` for diagnostics).

Persistent chats with at least one compatible replica take a transactional
opaque checkpoint of every selected range at a completed turn. Origin-only
swarms avoid that copy. On a failed RUN, the gateway may open a clean session
on the same executor (including through its relay), or select a compatible
replica with exactly the same layer boundaries/schema/numeric class. It then
restores the common checkpoint when present, otherwise replays from token zero,
and retries the failed batch. No model-specific KV layout enters Lumabri and a
possibly partial session is never reused. If no exact-range recovery route can
be opened, the error includes the executor status instead of presenting every
engine rejection as a dead peer.

## Encryption

Set `LUMABRI_ENCRYPT=1` on tracker, executors and chatter to use Lumabri's
X25519 + ChaCha20-Poly1305 transport with persistent/TOFU or pinned peer
identities. `LUMABRI_PEER_KEY`, `LUMABRI_KNOWN_HOSTS` and
`LUMABRI_PEER_PINS` have the same meaning as on the existing data plane.

## Release gate

`test-segment-direct-real` requires the same twelve model/reference variables
as Colibri's Edge oracle gate:

```sh
make test-segment-direct-real ENGINE=/path/to/colibri/c \
  GLM_EDGE_MODEL=... GLM_EDGE_REF=... \
  INKLING_EDGE_MODEL=... INKLING_EDGE_REF=... \
  KIMI_EDGE_MODEL=... KIMI_EDGE_REF=... \
  OLMOE_EDGE_MODEL=... OLMOE_EDGE_REF=... \
  QWEN_EDGE_MODEL=... QWEN_EDGE_REF=... \
  DEEPSEEK_EDGE_MODEL=... DEEPSEEK_EDGE_REF=...
```

Run the same command with `LUMABRI_ENCRYPT=1` to gate encrypted control and
data planes. The target also runs the sampler unit gate, a relay-only real
session, checkpoint/replay after a killed executor, and ordinary
`lumabri serve` + `lumabri chat` with context negotiation.

`make test-segment-hybrid ENGINE=/path/to/colibri/c` is the focused hybrid
gate. It establishes an oracle with a local OLMoE Segment, joins a strict-RAM
donor holding one complete layer, requires non-zero donor EXEC calls and the
same tokens, then kills the donor and requires the local fallback to produce
the oracle again.

## Remaining operational scope

- The archive advertises CPU only. GPU Segment adapters must expose a real
  Colibri backend before Lumabri may publish CUDA/HIP/Metal/Vulkan capability.
- The current governor is conservative rather than complete: CPU/RAM/context
  choose chunk/session limits, desktop donors are niced and Segment donation
  is refused when its estimated range would consume the system reserve. Live
  pressure-triggered migration is still pending.
- Relay is a reachability floor, not the fastest topology: it adds a tracker hop
  and currently serializes requests per executor tunnel. The queue wait is
  bounded and unsigned callers are rate/concurrency limited per observed
  source; this is abuse containment, not caller authentication. Reachable peers
  should publish direct P2P; multiplexing and decentralized relay/hole punching
  remain future work.
- One chatter process hosts one active Edge model. This also respects Qwen's
  currently process-global tokenizer state.

These are roadmap items, not hidden fallbacks. Existing gates prove the common
Segment path and the explicitly exercised adapters; they do not yet prove a
real two-machine LAN, disk mode, or conformance of every current family.
