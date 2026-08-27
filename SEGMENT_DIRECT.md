# Direct Segment execution

Lumabri uses Colibri's public Edge/Segment ABI to run one model as a
chain of layer-aligned peers:

```text
prompt -> local Edge -> peer A [0:k] -> peer B [k:n] -> local Edge -> token
```

The implementation is model-neutral. The release gate runs the same direct
TCP path for GLM, Inkling, Kimi K3, OLMoE, Qwen3.6 and DeepSeek V4. Every
family is split over two peers and three generated token IDs are compared with
its independent Colibri tiny-model oracle. The gate also overlaps two OLMoE
chats against the same executors to exercise real session isolation, and
retransmits a committed run to prove the cached duplicate response is exact.

This is now the preferred data plane of the ordinary `lumabri chat` TUI. If
the runtime, identity or a complete compatible route is absent, startup falls
back automatically to the existing expert/CAS Colibri path. Colibri's ordinary
executables remain unchanged.

## Build

Use a Colibri checkout containing the additive Edge runtime:

```sh
make ENGINE=/path/to/colibri/c
```

When the two additive ABI headers exist, ordinary `make` includes
`segment_node` and `segment_chat`; older Colibri release trees keep the legacy
build. `segment-edge-library` contains both public runtimes and all six
adapters, and is not linked into ordinary Colibri executables.

## Normal use

The model owner runs:

```sh
lumabri serve --model /models/olmoe --advertise PUBLIC_IP
```

Lumabri detects the family, layer/context shape, CPU/RAM and public IPv4, then
starts four disjoint fallback executors. A public interface address removes the
need for `--advertise`; private/NAT addresses are never guessed.

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
after the machine reserve and has a reachable direct address; otherwise
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

Open the Segment port on each executor's firewall. The current transport is
direct TCP, so an executor behind an unforwarded NAT is not reachable yet.

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
data planes.

## Deliberate current limits

- Edge v1 selects greedy tokens only. Temperature, top-p and top-k are not
  silently approximated outside Colibri.
- Segment relay and NAT hole punching are not implemented; only tracker
  discovery and direct peer connections are used.
- Snapshot bytes are not transported yet. If a selected peer dies, the chat
  stops with an explicit checkpoint/replay error rather than producing a
  divergent continuation.
- The archive advertises CPU only. GPU Segment adapters must expose a real
  Colibri backend before Lumabri may publish CUDA/HIP/Metal/Vulkan capability.
- The current governor is conservative rather than complete: CPU/RAM/context
  choose chunk/session limits, desktop donors are niced and Segment donation
  is refused when its estimated range would consume the system reserve. Live
  pressure migration and state replay are still pending.
- One chatter process hosts one active Edge model. This also respects Qwen's
  currently process-global tokenizer state.

These are roadmap items, not hidden fallbacks. The completed milestone proves
one RTT per segment, exact multi-session state ownership and real token
generation over the network for every current model family.
