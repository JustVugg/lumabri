# Try a native household candidate

This is a test candidate, not certification that the complete roadmap is done.
It contains the C workspace, tracker, checkpoint source, Segment services,
diagnostic probe and weight loader. It contains **no model weights or keys**.

Keep the whole folder together. On macOS, open `Lumabri.command`; on Linux,
run `./start-lumabri` from a terminal. The launcher opens the normal TUI.
Python, pip and the Colibri source checkout are not needed to run the folder.

1. On one computer, use `/create`. On the others, use `/join`, check the
   displayed identity and enter that household's key.
2. On helpers, open **Share resources** and keep it open. They do not allocate
   model memory until their owner accepts a particular request.
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
- Neither a GPU detector nor a manifest entry proves accelerated execution.
  These candidates use the verified CPU path. Each participating machine
  must permit the household's fixed LAN ports in its own firewall.
- One household chat plan per participating computer and cache directory.
  Disjoint donor groups can serve independent chats concurrently; this is
  not multi-session inference on a shared donor. Losing a participating donor stops the
  current chat and releases resources; automatic household replay is pending.

## Building and testing a candidate

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

`make install-household PREFIX=/your/prefix` is the strict install target:
all required services must build, not just the TUI. The older general install
target remains available for existing Expert/public-network deployments.
