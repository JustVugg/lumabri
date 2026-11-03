# Try a native household candidate

This is a test candidate, not certification that the complete roadmap is done.
It contains the C workspace, tracker, checkpoint source, Segment services,
diagnostic probe and weight loader. It contains **no model weights or keys**.

Keep the whole folder together. On macOS, open `Lumabri.command`; on Linux,
run `./start-lumabri` from a terminal. The launcher opens the normal TUI.
Python, pip and the Colibri source checkout are not needed to run the folder.

If startup or sharing fails, run `./Lumabri.command doctor --household` on
macOS, or `./start-lumabri doctor --household` on Linux. This tests that both
installed Segment executables can actually start, including their linked
library loading; it does not load model weights or start background services.
Keep any loader error printed before the report. Reinstall a complete package
if a runtime is missing or incompatible; do not mix individual binaries from
different downloads. `--json` provides the same results for diagnostics.
Passing this check does not prove model compatibility or connectivity to peers.

1. On one computer, use `/create`. On the others, use `/join`, check the
   displayed identity and enter that household's key.
2. On helpers, open **Share resources** to review requests. They do not allocate
   model memory until their owner accepts a particular request or grants an
   explicit bounded repeat policy. Esc detaches the view; it does not stop sharing.
3. On the chatting computer, set the source model folder in `/settings`,
   open **Your computers** and select the helpers you want to use.
4. Choose a supported, fitting model and request its plan. Accept on each
   selected helper. Chat starts after the complete approved chain is ready.

The source checkpoint is still required on the requesting computer. A synthetic
Tiny fixture tests transport and execution; its random text and speed do not
represent a trained large model. An unmeasured model has no numerical speed.
The latest measured speed is historical, not a guarantee for the next turn.

## Platform limits

- The manifest names the actual native build OS and architecture. An Intel
  archive is not an Apple Silicon archive. macOS CI runs do not prove macOS
  12.6 compatibility; test that machine before claiming it supported.
  CI targets macOS 12.0, and packaging checks the minimum OS recorded in
  every Mach-O binary. This necessary compatibility check is not a substitute
  for running the candidate on macOS 12.6.
- macOS CI builds both a single-thread variant and a multicore candidate.
  The latter bundles LLVM OpenMP 20.1.8 from pinned source at
  `87f0227cb60147a26a1eeb4fb06e3b505e9c7261`, built for the macOS floor.
  It does not require Homebrew on the helper. Copies are relocated to
  package-relative library paths and ad-hoc signed after relocation; source
  binaries are not modified. Actual performance still needs a LAN comparison.
  A Mac downloaded archive
  can require approval in macOS security settings; it is not notarized.
  Spaces in the install folder are tested; colons in macOS library paths are
  rejected because the loader treats them as a library-list separator.
- Linux records its dynamic library dependencies in `manifest.json`; it is
  not a universal static binary. Use the recorded compatible OS/runtime.
  WSL runs this Linux candidate. The separate native Windows package has its
  own executables, launcher and capability limits.
- For native Windows installation and update instructions, use
  [Windows package](WINDOWS_PACKAGE.md). Its shared C catalogue supports
  explicit donor selection, separate approvals, resident chat, a persistent
  service and multiple model reservations. The local engine currently executes
  CPU OLMoE; other adapters and GPUs are not advertised as local capabilities.
  It does not yet host the HTTP/web API. A Linux/macOS gateway can serve a
  mixed household through the existing authenticated protocol.
- Windows acceptance uses real PE executables and private disposable profiles:
  ConPTY navigation, encrypted tracker/source/donor, identity-pinned chat,
  separate conversations, eight-token calibration, cancellation, co-resident
  models, manager restart, installed update and rollback. Reduced checkpoints
  exercise execution and lifecycle; they do not establish large-model speed.
  Checkpoint paths must be local; private credentials refuse shared drives,
  hard links and reparse paths. Owned Job Objects and engine page locks enforce
  process lifetime and resident memory. No system-wide memory limit, firewall
  or execution policy is changed by these tests.
- Neither a GPU detector nor a manifest entry proves accelerated execution.
  These candidates use the verified CPU path. Each participating machine
  must permit the household's fixed LAN ports in its own firewall.
- The background donor can retain up to four independently approved resident
  CPU Segment allocations, each reserving 1–8 isolated conversation slots.
  Their total memory must fit the sharing budget; a slot is not a single-user
  speed guarantee. Compute admission is bounded across models. Stopping one
  allocation must not unload the others.
- Ordinary household chat does not automatically replace a lost donor.
  Managed API models can use explicitly configured replica policies and bounded
  visible replay. Recovery still needs an authorized compatible replacement,
  sufficient capacity and unchanged permissions. No transparent KV migration
  or uninterrupted-answer guarantee is made; see [replica policies](ELASTIC_REPLICAS.md).
  Failure does not justify assuming a disconnected donor's RAM was released.

## Building and testing a candidate

The native Windows worker developer gate requires MinGW-w64/UCRT and the
same pinned Colibri sources. It downloads no checkpoint and does not install
services. From a native build shell, with an existing four-layer tiny OLMoE
checkpoint:

```sh
python tools/build_windows_segment.py --engine /path/to/colibri/c --only inference
build/windows-segment/test_segment_worker_windows.exe C:/absolute/path/to/segment_node.exe C:/absolute/path/to/tiny-olmoe selftest
build/windows-segment/test_segment_worker_windows.exe C:/absolute/path/to/segment_node.exe C:/absolute/path/to/tiny-olmoe remote-selftest C:/absolute/path/to/segment_chat.exe
```

The worker executable is also in `build/windows-segment`. The test launches
it in an owned process job, uses disposable local identities and ports, and
releases its resources on exit. This is a worker acceptance gate, not a
Windows household installer or a speed benchmark.

Build Colibri from the pinned checkout used by CI, without editing it. Build
`make household swarm_probe ENGINE=/path/to/colibri/c`. For a macOS candidate,
pass `OMP_FLAGS= OMP_LIBS=` for the single-thread variant. For multicore,
`tools/build_macos_openmp.py` takes a clean pinned LLVM checkout and new
build/install directories; supply its install prefix as `OMP_PREFIX` to make
and `--openmp-prefix` to the packager. CI demonstrates the full sequence and
tests both Intel and Apple Silicon installed layouts. The archive is still
not Developer-ID signed or notarized.

Then assemble a **new** directory:

```sh
python3 tools/package_household.py --colibri-root /path/to/colibri --output /tmp/lumabri-candidate
python3 tests/integration/home_flow_test.py --runtime-dir /tmp/lumabri-candidate/bin --models-dir /path/to/tiny-model-parent --repeat-cached --expect-metrics --expect-calibration
python3 tools/package_household.py --verify /tmp/lumabri-candidate
```

The test uses fresh isolated homes, real encrypted services, two approvals,
two actual Segment ranges, streaming, cache reuse and invalidation of timings.
It does not use binaries from the source directory. Keep its logs together
with the candidate manifest. Run the lost-donor test separately with
`--kill-donor`. CI uploads candidates only after their installed-layout tests
succeed; physical two-computer LAN validation remains a separate release gate.
The post-test hash check rejects missing, changed or extra files and symlinks.
It checks consistency with the manifest, not the trustworthiness of a download
or a cryptographic release signature.

For the managed multi-model service, also run the installed-package gate:

```sh
LUMABRI_RESIDENCY_POLICY=engine-locked \
python3 tests/integration/household_service_flow_test.py \
  --installed-package /tmp/lumabri-candidate --models-dir /path/to/tiny-model-parent \
  --joint complete --api-prepare --donor-ram-gb 1.5 --sessions 2
```

It uses only shipped production binaries, never a copied `test_chat_ui` or a
replacement engine. It verifies two models on one donor, real mixed requests,
bounded overload, client Stop, detached TUIs, manager/API restart, private
history and selective release without replacing the other model's engines.
The test requires the package's native OS/architecture and clean build source
metadata, and rechecks its contents afterward. See
[the release gate](https://github.com/JustVugg/lumabri/blob/main/PRODUCTION.md#installed-gateway-update-and-rollback) for
report scope and the separate update/rollback checks.

Updating the whole household requires a different check from changing only
its API. Follow the
[installed update procedure](https://github.com/JustVugg/lumabri/blob/main/docs/INSTALLED_UPDATES.md):
retire allocations, stop all services, switch complete packages and obtain
fresh approvals. `service restart` alone leaves old keepers and engines alive.

`make install-household PREFIX=/your/prefix` is the strict install target:
all required services must build, not just the TUI. The older general install
target remains available for existing Expert/public-network deployments.

### Native donor screen and signed inventory

In the developer build, start `lumabri.exe` without arguments. Choose **Join a
household**, enter its address and key, then use **Settings** to set the model
folder and maximum RAM contribution. The key is hidden while typing. Joining
checks encrypted authentication before saving; a failed connection or wrong
key leaves the previous household unchanged. Settings live in a private,
atomically replaced `%LOCALAPPDATA%\Lumabri\home.conf`; Unicode paths are
supported. Use **Share resources** to start the background donor and review
incoming requests. An active donor must stop before joining another household.

For scripted use, `lumabri donor --join HOST:PORT --ram-gb N` still starts
sharing with `LUMABRI_TOKEN` set. `--headless` starts just the keeper;
`lumabri share` starts or reopens sharing using saved settings. Arrow keys select Accept or
Decline; Enter confirms. Esc detaches without unloading, `x` releases the
selected model and `s` stops sharing. Repeat-load policies are not offered
in this native build.

The keeper publishes authenticated, signed machine observations independently
of the view, including available budget, runtime identity, thread capacity,
reserved memory and compute queue. Inventory wire version 6 adds exact adapter
roles. Update the tracker and requester together: older trackers reject the
new report. Updated readers still accept version 3–5 observations with unknown
roles. Native Windows advertises OLMoE Edge and Segment on CPU; it cannot be selected
as Hybrid, and unavailable roles never become available just because
there is spare RAM. This is not a GPU execution claim.

The Windows gate drives the real `lumabri.exe` through ConPTY: joins the
authenticated household, edits the RAM limit, starts sharing, approves a
resident allocation, checks resize handling, detaches and reopens with the same
worker still alive. Signed inventory, independent model release, hosted chat
and 72 oracle matches are checked in that same isolated deployment. Separate
launcher tests cover wrong keys, masked credentials, Unicode input, cancellation,
reopening and invalid settings.

**Create a household** starts the common tracker under a private native keeper.
The workspace saves its address and key only after readiness and an encrypted
identity check. The key is hidden until explicitly revealed. A port conflict
leaves existing settings intact. Closing the workspace keeps the tracker alive;
reopening shows the same pairing details, and restarting the manager preserves
it. **Stop local services** releases this computer's keepers.

The native tracker gate checks that complete lifecycle, LAN discovery hints,
authenticated signed Segment registration and route-generation advancement
after a tracker restart. The shared durable-history gate also runs as a real
Windows process: cache overflow, legacy records, competing publishers, key
rotation and corrupted evidence. Installed Windows candidates also run the
catalogue-to-chat flow and quiescent package update/rollback gate.

## Native Windows resident chat client

The developer build now includes a thin interactive client for an already
approved host. It uses the same prompt templates, greeting validation and
Serve2 reply parser as the Unix client. It does not start or download a model.
Pass the endpoint, public identity and checkpoint root from the approved plan:

```text
lumabri.exe chat --host HOST:PORT --host-key PUBLIC_KEY --host-root CHECKPOINT_ROOT
```

The client uses the saved household credential (or an explicitly configured
`LUMABRI_TOKEN`). An identity or checkpoint mismatch refuses the connection
before sending conversation text. A full host reports BUSY. The text transcript
stays in this client session; completed turns are replayed on reconnection.
Esc or Ctrl-C during a reply closes that connection and excludes its partial
reply from subsequent prompts. `/new` starts a fresh conversation, `/quit`
returns, Tab completes commands, and Ctrl-P/Ctrl-N scroll the transcript.
Measured speed appears only when the host returns generation timing.

Native tests cover the actual console, encrypted cancellation and interrupted
streams, and two turns against a real reduced OLMoE host with no further weight
reads. The workspace catalogue handles model preparation; this command only
attaches to an already approved host.

### Native Windows catalogue preparation

The native workspace can select computers, review a resident model plan, and
start a persistent preparation keeper. Every participating donor must approve;
local approval is available in the preparation screen. Closing the workspace
leaves a pending preparation or READY model under service ownership. Reopening
Explore models reconnects to the operation instead of starting a duplicate.

The catalogue also reviews a joint plan for selected models. Each model gets
its own approval and reservation; this is a sequence, not an atomic transaction.
If a later model is refused or cancelled, models already READY remain in Resident
models. Cancelling preparation does not unload an earlier completed allocation.

Resident models opens a new private conversation without downloading or loading
weights again. Completed turns update the common provenance-bound calibration
record. **C measure** runs at most eight output tokens with a 20-second generation
deadline on that same resident plan. Model, executable, hardware, runtime,
interval, context, session and managed-workload changes invalidate the saved
number. A short observation does not certify concurrent serving capacity.

`test_requester_windows.exe` drives the production native console, tracker,
signed source, donor and Edge through this path using a reduced OLMoE checkpoint.
It covers pending-operation detach/reopen, explicit approval, retained workers,
manager restart, conversation isolation, calibration, cancellation and joint
preparation. It requires complete inference binaries from
`tools/build_windows_segment.py`, not `--only contracts` alone. The installed
package gate reuses this flow, then switches to a separately built version and
back with fresh approvals and chat after each switch. Physical mixed-machine
acceptance is still a separate release check.
