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
  Windows/WSL runs this Linux candidate. Native Windows inference is not
  included and is not certified by the firewall helper.
- Separate native Windows developer gates exercise resident OLMoE, the Segment
  protocol, encrypted TCP and persistent peer identities. These are not a
  packaged Windows household service. The native identity store defaults to
  `%LOCALAPPDATA%\Lumabri`, using protected owner/System ACLs and bounded file
  locks. Explicit credential paths must also be private local files; shared
  drives, hard links and reparse paths are refused. Existing corrupt or unsafe
  keys are never silently replaced. The gate uses disposable files and does
  not change installed identities or firewall settings.
- Native Windows local service control uses the same versioned snapshots,
  preparation records and mutation fences as Unix. Its acceptance test uses
  separate Windows processes: private local named pipes, duplicate-keeper
  refusal, bounded malformed input, independent client exit, manager restart,
  atomic journal replacement and crash recovery. A journal never grants
  permission to replay an approval. This gate does not yet deliver the full
  native Windows household service or package.
- The per-donor compute permit also has a native Windows implementation:
  private same-user named pipes, a bounded FIFO, queue deadlines and
  cancellation. The connected process owns its permit; exit releases it.
  Native tests cover overload, invalid requests, Unicode profile paths,
  separate-process crashes and repeated restart without leaked handles.
  The keeper passes this broker to approved workers; a compute permit alone
  never authorizes loading a model.
- The shared residency guard now checks native Windows engine locks and input
  sealing during real OLMoE generation. Inherited worker control and reserved
  TCP listener handoff also have native process tests, including incomplete
  replies, stale mutations and a mismatched port. Listener handoff uses
  Winsock duplication for the owned child; it never rebinds a released port.
  The production `segment_node.exe` now uses these controls and the same
  encrypted Segment protocol. Its developer build explicitly links OLMoE
  only; capability queries do not advertise unlinked adapters. An approved
  process budget sets its Windows working-set allowance, while owned page
  locks, input sealing and the process guard enforce residence. No system-wide
  memory limit is changed. A native test compares 72 generated tokens with a
  local Colibri oracle across separate conversations and drain/resume/retire.
- Native Windows worker preparation can receive a signed checkpoint from the
  approved LAN source without preinstalling its weight files. Bounded metadata
  and safetensors headers are materialized in private sparse files; engine
  weight reads use verified chunks directly from the source. The input bridge
  rejects Windows path aliases, wrong signing keys, altered chunks and unknown
  file handles, then closes network input after resident preparation. Its
  eight-chunk cache is temporary and is released before readiness. This is a
  worker developer gate, not a Windows household package; the gate compares
  72 tokens against the local oracle and
  checks that generation makes no further source reads. Native engine builds
  embed a [process-local UTF-8 code-page manifest](https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests#activecodepage)
  (Windows 10 version 1903 or newer); preparation is exercised in a path with
  spaces and non-ASCII text.
  That test uses a signed lease fixture; a separate Windows/Linux loopback
  test also exercises the production tracker and Linux Edge.
- Native `segment_chat.exe` uses the same Edge, discovery and SUBMIT/DATA/DONE
  implementation as Unix. Its Windows input bridge verifies the approved
  checkpoint, admits the explicit `LUMABRI_EDGE_RAM_BYTES` working-set budget,
  and seals weight input before readiness. Binary pipes preserve embedded LF
  and control-Z bytes. The native gate compares 24 generated token IDs with a
  local oracle and exercises two conversation slots, reset, cancellation and
  reuse, malformed control, EOF, and no further checkpoint-source reads after
  readiness. This is a native engine gate; the donor still refuses Edge offers
  until the host gateway and approved allocation lifecycle are connected.
- The native `lumabri.exe` control entrypoint can now start, inspect, restart
  and stop the private background manager with `service`, and report the
  Windows machine inventory with `machine --json`. The shared manager loop
  observes existing keepers without replaying approvals. Its native acceptance
  test starts eight clients concurrently, closes the launchers, restarts and
  crashes the manager, and verifies that a separate keeper retains its state.
  This is a developer control entrypoint, not a complete Windows household
  package. Build it with
  `python tools/build_windows_segment.py --only contracts`; the executable is
  in `build/windows-segment`. Unsupported commands fail explicitly.
- The native donor keeper now accepts encrypted household offers and waits
  for approval through the same private, revision-fenced service API as Unix.
  Each approved allocation owns a resident OLMoE worker, reserved listener,
  memory reservation and immutable lifetime record. Up to four allocations
  share one bounded compute FIFO; closing the requester or restarting the
  manager preserves prepared workers. Release waits for process and inherited
  handle exit. Terminal receipts can be reconciled after an interrupted reply;
  old allocation IDs and approvals are never replayed.
  The native gate prepares two allocations, compares 72 oracle tokens, retires
  one without interrupting the other, and checks that inference makes no further
  checkpoint-source reads. Separate tests check inherited lifetime fences,
  missing/corrupt evidence and receipt replay.
  This developer entry requires a reachable authenticated tracker and owner
  approval. The native screen and signed inventory are described below.
  Repeat policies, native Edge hosting and a complete Windows package are
  not delivered by this gate. Unsupported Edge/Hybrid or model
  allocations are explicitly refused; registering adapters is not sufficient
  to claim their native execution.
- Native engine ownership requires Windows 10 version 1607 or later. The
  supervisor assigns the process tree to its private Job Object atomically at
  creation, passes only explicitly selected handles and signals readiness with
  an inherited event. Stop confirms the entire tree has exited; a saved PID
  never authorizes termination. Native tests cover owner crashes, descendants,
  cooperative stop, Unicode paths and arguments, and resident OLMoE inference
  under this supervisor. Capability probes use the same bounded ownership path.
- The shared C catalogue also has a native Windows console backend. A ConPTY
  test (Windows 10 1809 or later) drives actual navigation, donor selection,
  model review, resize and Ctrl-C, and checks restoration of console modes and
  code pages. Checkpoint inspection accepts UTF-8 paths and 64-bit file extents;
  its Windows gate verifies bounded reads of a sparse shard larger than 4 GiB.
  These gates do not replace native chat hosting and final package validation.
- Native Windows inventory uses physical/available RAM bounded by commit and
  visible Job Object limits, processor affinity, physical cores, NUMA, DXGI
  hardware detection and free disk space. A shared job memory ceiling uses the
  reported peak as a conservative bound; it can understate released capacity.
  Detection does not enable a GPU backend or add shared system RAM as VRAM.
  The native governor test checks bounded memory, single-CPU affinity, exclusive
  donor leases, private pause state and recovery without changing user settings.
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

In the developer build, `lumabri donor --join HOST:PORT --ram-gb N` starts
sharing and opens the same C approval screen used on Unix. Set the household
key in `LUMABRI_TOKEN` before starting. `--headless` starts just the keeper;
`lumabri share` reopens its approval screen. Arrow keys select Accept or
Decline; Enter confirms. Esc detaches without unloading, `x` releases the
selected model and `s` stops sharing. Repeat-load policies are not offered
in this native build.

The keeper publishes authenticated, signed machine observations independently
of the view, including available budget, runtime identity, thread capacity,
reserved memory and compute queue. Inventory wire version 6 adds exact adapter
roles. Update the tracker and requester together: older trackers reject the
new report. Updated readers still accept version 3–5 observations with unknown
roles. Native Windows advertises only OLMoE Segment on CPU; it cannot be selected
as Edge or Hybrid, and unavailable roles never become available just because
there is spare RAM. This is not a GPU execution claim.

The Windows gate drives the real `lumabri.exe` through ConPTY, approves a
resident allocation, checks resize handling, detaches and reopens with the same
worker still alive. Signed inventory, independent model release and 72 oracle
matches are checked in that same isolated deployment. This covers the donor
workflow; native household setup, chat hosting and final packaging are separate.
