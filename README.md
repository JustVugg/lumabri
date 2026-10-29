<img src="logo.svg" alt="lumabri: tiny engine, immense swarm" width="524">

# Your computers. Your models.

Lumabri runs a private inference service across your computers, powered by
[Colibri](https://github.com/JustVugg/colibri). Choose models, select the
computers that may help, and approve how their memory is used. Chat from the
terminal or the bundled web page. The controller, terminal interface and
inference integration are written in C; the browser uses bundled HTML/CSS/JS.

**A worker does not need its own complete copy of the model.** In the resident
Segment path, it loads the weights for its assigned layers, plus any shared
state required by the adapter. Conversation state stays with the nodes that
execute those layers. A complete source checkpoint may still live on the
requesting computer; distributing its weights does not duplicate it in every
worker's RAM.

- Run supported models that do not fit on one computer by sharing the work.
- Keep several approved models resident within explicit memory budgets.
- See computers, allocations and measured workloads in one local workspace.

More computers can add usable memory and capacity. They do not guarantee more
tokens per second for each chat. Lumabri measures performance rather than
promising that every additional device makes inference faster.

![The household workspace](docs/images/workspace.png)

## Start here

Build from source with a compatible Colibri checkout:

```sh
make household swarm_probe ENGINE=/path/to/colibri/c
./lumabri
```

After launch, use **arrow keys and Enter**. Press **/** for workspace actions
and **Esc** to go back. Use a terminal at least 60 columns by 28 rows.

1. On one computer, open **/ → /create**. Its household service runs in the background.
2. On your other computers, open **/ → /join**. Select the household found
   on your LAN, compare its identity with **/create** on the owner computer,
   then enter the household key. Manual address entry remains available.
   Only share that key with your household.
3. Open **/settings** to set the maximum RAM you offer. Choose
   **Share resources** on each computer that may participate.
4. On the requesting computer, set **/settings** to the folder containing
   your existing Colibri model directories. Set the **context tokens** for
   new plans there too (default 512): longer conversations need more memory.
   This setting does not resize models that are already running.
5. Open **Your computers**. Press Enter to select donors; none is selected
   automatically. Tab switches to **Models**.
6. Choose a model and review its plan. Enter requests approval from the
   participating donors. Chat starts only after all approve and the complete
   chain is ready.

The creating computer saves its key and restarts its household when Lumabri
reopens. The tracker uses **TCP 47300**, household services use **TCP
47301–47315**, and LAN discovery uses **UDP 47300**. Restarting does not require
opening another random port. If the fixed range is occupied, Lumabri reports
the conflict instead of silently choosing ports outside it.

The computers still need direct LAN connectivity. On Windows with mirrored
WSL networking, open **/network** on the Windows computer: after your explicit
approval and the Windows administrator prompt, it allows only those ports
from the detected private subnet. The firewall stays enabled. The same action
can remove Lumabri's rules. Keep the `tools` folder beside the built binary.
Other systems use their normal firewall permissions. Guest Wi-Fi isolation,
VPN routing and WSL NAT mode can still prevent direct discovery or connections;
the helper does not reconfigure those networks. If your LAN address changes,
use **/join** to rediscover the household rather than guessing an address.

For an advanced multi-interface setup (for example, a VPN or Tailscale), start
the household owner with `LUMABRI_ADVERTISE=100.101.102.103 ./lumabri`, replacing
the example with that computer's reachable IPv4 address. Use the same override
on later starts; Lumabri saves the announced tracker endpoint in `home.conf`.
Join that exact endpoint on the other computers. The value is an IPv4 address
without a port, not a hostname. This does not configure VPNs or firewall rules.
An address mismatch is rejected explicitly; it does not bypass household
authentication or donor approval. Reopened TUIs restore their saved household key.

Apple Terminal uses a compatible 256-colour palette with a contrasting
selection background. Other terminals use detected colour support. For
diagnostics, `LUMABRI_COLOR=16`, `256`, `truecolor` or `none` overrides detection;
`NO_COLOR=1` disables the canvas colours. Normal use needs no colour command.

## A plan before a download

![A real catalogue frame from the tiny-model integration test](docs/images/models.png)

The catalogue reads the model folders and the household inventory. It shows
offered RAM, detected hardware, placement and missing resources. **Not
calibrated** means there is no matching speed measurement—not zero speed.
A detected GPU is not a promise that this execution path uses it.

For an optional short measurement, select your computers and model, open
**/ actions → /calibrate**, then review and confirm. Donors still approve the
plan. Lumabri generates at most 8 test tokens, with a 20-second inference
limit, saves the observed speed and releases the plan. Transferring and
loading missing weights happens first and can take much longer. This is a
short-run indication, not a guarantee for long conversations.

Each participating computer can retain independently budgeted **CPU, resident
Segment allocations**, with verified model sizing and source weights on the
requesting computer. It
distributes contiguous layer ranges, not isolated experts. Each donor keeps
the state for its layers; the chosen chat host receives the conversation text.
The chat connection itself does not mount or download a checkpoint.
Up to four model allocations can coexist on one donor within its total budget;
each plan explicitly reserves 1–8 isolated conversation slots. Compute uses
bounded queues, not a promise that every slot runs at the single-user speed.
Exhausted capacity returns `BUSY` rather than replacing a running model.
An ordinary household plan does not automatically replace a lost donor.
For managed API models, opt-in [replica policies](docs/ELASTIC_REPLICAS.md)
can restore capacity within approved limits after repeated availability
failures. Preparation still requires donor permission and spare capacity;
this does not promise an uninterrupted response.
The web chat can wait up to two minutes for an approved replacement and replay
an interrupted response, checking the text already shown before continuing.
Stop cancels the wait; insufficient capacity preserves an incomplete answer.

The donor queries its installed Segment runtime before sharing. A runtime
built without OpenMP uses one execution thread; CPU core count is not a
promise of engine parallelism. The default system RAM reserve is one quarter
of physical RAM, bounded to 1–4 GiB (2 GiB on an 8 GiB computer). The sharing
limit is an additional cap, not permission to consume the system reserve.

Adding computers can make a model fit when it would not fit on one machine.
It does not automatically make each chat faster.

## Sharing is your decision

![A real donor request, defaulting to Decline](docs/images/approval.png)

A request shows the model, layer range, RAM budget, disk headroom and whether
the computer also hosts chat. **Decline is selected initially.** Move to
Accept and press Enter only when you agree.

No model is loaded until every participating donor accepts. Rejection or
cancellation during preparation releases incomplete allocations. **Esc closes
the view, not the service.** Approved resident weights survive closing chat or
the TUI. In Share resources, **x** unloads the model and **s** stops sharing.
**Resident models** lists your saved plans: **r** checks every approved donor,
Enter opens a conversation, and **x** asks for confirmation before unloading.
A saved plan is not presented as live until its donors confirm it.

Use **/service** to inspect or explicitly stop local services. Restarting the
manager with `lumabri service restart` leaves live allocation keepers running;
it never replays a saved approval. See [service lifecycle](docs/HOUSEHOLD_SERVICE.md)
for restart behavior and current limits.

Inside chat, your messages and the streamed answers stay in the transcript.
Type **/** for command suggestions, use arrows to choose and Tab to complete.
**/help**, **/debug**, **/reset** and **/quit** provide help, diagnostics, a new
conversation and exit.

### Browser chat

![Lumabri's bundled browser chat, showing an example conversation](docs/images/web-chat.png)

Prepared resident models are also available through the authenticated local
API and its bundled chat page. An operator creates a user token, grants it
specific models or allocations and runs `lumabri api start`; open the printed local URL.
For example, after preparing a model in the TUI, run these commands on its
requesting computer, using the same Lumabri configuration:

```sh
./lumabri api list
./lumabri api user-add local-chat
# Replace MODEL_ID with the ID shown by api list.
./lumabri api grant local-chat MODEL_ID
./lumabri api start
```

Keep the token printed by `user-add` private and enter it on the chat page.
Choose the granted model and send a message. This account can chat with that
model; it cannot approve donors or administer the cluster. For the optional
read-only **Computers & models** view, explicitly grant
`./lumabri api operator-grant local-chat`.

The page provides model selection, streaming, Stop, collapsible private
history and export. No separate web server or browser-side AI engine is needed.
During a response, the speed indicator updates in tok/s once enough tokens have
arrived. It shows the measured response rate, not the model's guaranteed capacity;
preparation, recovery and unavailable measurements have separate labels.
See [setup, limits and privacy](docs/RESIDENT_API.md). The default endpoint is
loopback-only: do not expose plain HTTP directly. For other authorized
computers, use the explicit [HTTPS reverse-proxy setup](docs/HTTPS_INGRESS.md).

**Computers & models** shows resource reports, resident allocation references,
model routes and gateway activity. Explicit management permission adds model
preparation, donor approval requests, replica policies, draining and release.
Read-only operators cannot change allocations.

**Measure & compare** runs a bounded workload on models already approved and
loaded. Choose allocations, client count and rounds; then check first-text and
token-gap limits. A changed configuration makes old evidence unusable. Applying
a measured plan requires a separate confirmation and replaces future-turn
routing atomically. It does not unload weights or shut down computers.

![Measured two-model plans and explicit routing review in the web workspace](docs/images/web-capacity.png)

These are screenshots of the compiled web interface. The chat text is an
illustrative example, not a model-quality benchmark. The comparison comes from
a real reduced-OLMoE loopback integration run; its machine prices are test
inputs, not cloud bills, demonstrated savings or production capacity figures.
Browser history is private between API users, but the service operator and
inference computers are trusted: this is not end-to-end encryption from them.

Household chat also shows the approved layer allocation. **/plan** displays
each compute donor, its layer range, reserved RAM and the Edge/chat host.
**/experts** queries activity on the same household tracker. Serving checkpoint
files is not the same as executing model layers; a chat process runs no layers,
even when a separate approved donor runs on that computer. The allocation is
a READY-time snapshot, not a live utilization or speed measurement.

## What has actually been checked?

CI builds and runs the CPU runtime on Linux x86-64 and ARM64, and native macOS
Intel and Apple Silicon. Its integration tests use real Colibri engines with
reduced checkpoints:

- An encrypted tracker, two donor TUIs, approval and rejection, hosted streaming,
  terminal navigation and resource release.
- Multiple resident models, isolated conversations, bounded concurrent requests,
  cancellation, selective unloading and service restart.
- The browser capacity path: measure, compare, confirm routing and chat through
  both models; permission checks and invalidation of stale measurements.
- Bounded replica policies and recovery/replay, including refusal when permission
  or capacity is missing.

Physical PC-to-Mac trials have also run OLMoE with different layer ranges on each
computer. Those observations apply to the tested package and configuration;
they do not certify every LAN, model family, context length or concurrent load.
The reduced-model tests establish runtime behavior, not large-model speed.
See the [release evidence](docs/LAN_RELEASE_CHECKLIST.md) for the recorded scope.

The native Windows household service and GPU household execution are not
delivered by these CPU checks. Windows/WSL testing is not native Windows
certification. ARM64 CI does not certify a physical Raspberry Pi cluster.
Recovery can require visible interruption and replay; it is not instantaneous
KV migration.

Adapter registration is broader than tested household execution. A checkpoint
appearing in the catalogue does not make its sizing or backend supported.
There are no promised tok/s or automatic “best model” recommendations.

### Native runtime build

Build the complete runtime with `make household ENGINE=/path/to/colibri/c`.
Building only `lumabri` does not install the model services. macOS builds use
Homebrew `libomp` when available and work single-threaded without it. Changing
OpenMP flags invalidates the generated engine build automatically. CI pins
Colibri to **v1.12.1**, commit
`ce370e87d7b623d7759b52ec2007d75fc5b0e87e`, for reproducibility.
Use that revision for local builds too. The eight upstream distributed adapters are
GLM, GLM5.3, Inkling, Kimi, OLMoE, Qwen3.6, Qwen3.8 and DeepSeek V4.
Lumabri also provides a [resident DeepSeek V4.1 text CPU adapter](docs/DEEPSEEK_V41.md),
including Engram and cross-layer state. Other standalone engines added by
Colibri are not automatically clusterable: each needs a verified adapter.

Discovery and outbound reporting do not prove inbound connectivity. Before
indexing, Lumabri checks each selected donor's address and identity. The
household tracker runs independently of the owner's window. Allow the installed
`lumabri` and `segment_node` executables through the donor's application
firewall rather than disabling the firewall. Rebuilding an unsigned executable
may require reviewing its application permission again.

## Authorized servers and containers

The same resident worker can run without a terminal under a process supervisor.
It keeps model approval, memory accounting and session limits; starting a worker
does not approve a model. See the [headless service](docs/HOUSEHOLD_SERVICE.md)
and [container worker deployment](docs/KUBERNETES_WORKERS.md) for the commands
and Kubernetes example.

The container gate exercises real reduced-model inference across separate
Kubernetes Pods, including a model that needs both workers' memory. This is
deployment evidence for that configuration, not a large-model throughput claim.

For administration, [MCP tools](docs/AGENT_CONTROL.md) let compatible agents
inspect the cluster through the same authenticated API. Management actions
require a separate grant and keep the existing memory budgets, review checks
and donor permissions. The inference service continues if the agent exits.

An optional [Kubernetes controller](docs/KUBERNETES_CONTROL.md) reconciles
existing, authorized replica policies. Kubernetes manages the containers;
Lumabri manages model placement and resident inference. The controller does
not create servers or grant access to a donor.

### Kubernetes: from workers to model policies

Start with a verified Linux package for your workers' architecture. Build the
resident image, configure the example with your tracker, household secret,
image digest and memory limits, then apply it in your own namespace:

```sh
python3 tools/prepare_container_context.py --package /path/to/linux-candidate --output /tmp/lumabri-image
docker build --tag lumabri-resident:local /tmp/lumabri-image
# Configure deploy/kubernetes/workers.example.yaml for your cluster first.
kubectl -n lumabri apply -f deploy/kubernetes/workers.example.yaml
```

The image must be available to the cluster. The namespace, authenticated
tracker and gateway must already be configured; the worker example is not a
complete cluster installer. Follow the [worker setup](docs/KUBERNETES_WORKERS.md)
for those prerequisites, approvals, networking and storage.

For declarative control of an existing Lumabri replica policy, install the
optional controller's API and namespaced permissions:

```sh
make lumabri-kube
kubectl apply -f deploy/kubernetes/replicapolicy-crd.yaml
kubectl -n lumabri apply -f deploy/kubernetes/rbac.yaml
```

Run `lumabri-kube` beside the gateway with the dedicated credentials and
arguments in the [controller setup](docs/KUBERNETES_CONTROL.md). Installing the
CRD alone does not activate it or authorize workers.

| What happens | Lumabri's behavior |
| --- | --- |
| A model needs more memory than one worker offers | A feasible approved plan can split its layers across workers. Chat waits for the whole chain to be ready. |
| Demand rises for an explicitly enabled replica policy | The service can prepare additional replicas on authorized machines, within that policy's limits and available resources. |
| Demand falls | The policy can drain eligible surplus replicas before release; active sessions are not discarded to free memory. |
| An administrator deletes a `ReplicaPolicy` | Automation pauses and resident weights remain. Deletion does not stop a server or unload the model. |
| A worker disappears | Configured recovery needs an authorized replacement and sufficient memory. Otherwise the conversation stays incomplete and the interruption is shown. |

Kubernetes supplies and supervises the Pods. Lumabri manages the resident
models and their sessions within them. Model replicas and Kubernetes Pod
replicas are different: Lumabri's controller does not create or delete Pods.

## Development and diagnostics

```sh
make test ENGINE=/path/to/colibri/c
python3 tests/integration/home_flow_test.py --models-dir /path/to/tiny-model-folder
```

The second command starts a loopback integration test with real engines; it
requires a compatible small OLMoE checkpoint. Normal household use stays in
the TUI.

See [repository layout](docs/REPOSITORY.md), [Segment engineering](SEGMENT_DIRECT.md)
and [release checks](PRODUCTION.md) for developer details. Expert/Hybrid,
public-network protocols and advanced CLI tools remain available for
development; they are not extra steps in the household interface.
