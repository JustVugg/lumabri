# Release archives

One archive per platform, built by `.github/workflows/release.yml`. Each holds
the Lumabri programs, the engine compiled into them, the web page compiled into
`lumabri`, licences and one file to double-click. No Python, compiler or model
is inside, and none is needed to run it. All archives are **CPU only**.

| Archive | Double-click | Other commands | Needs from the system |
|---|---|---|---|
| `lumabri-<version>-windows-x86_64.zip` | `START-HERE.bat` | `Lumabri.cmd` | Windows 10 1903 or newer |
| `lumabri-<version>-linux-x86_64.tar.gz` | `start-here.sh` | `./start-lumabri` | glibc 2.35 or newer; libcurl 4 for model download |
| `lumabri-<version>-linux-arm64.tar.gz` | `start-here.sh` | `./start-lumabri` | glibc 2.35 or newer; libcurl 4 for model download |
| `lumabri-<version>-macos-arm64.tar.gz` | `START-HERE.command` | `./Lumabri.command` | macOS 12 or newer |
| `lumabri-<version>-macos-x86_64.tar.gz` | `START-HERE.command` | `./Lumabri.command` | macOS 12 or newer |

The double-click file runs `lumabri start`: it creates this computer's
household if there is none, starts sharing and opens the local page in the
browser. On a computer with no model the page shows **Get a model**. Add
`--tui` for the terminal workspace or `--no-browser` to print the sign-in link.
`service stop` through the other launcher stops everything. The launchers are
plain shell or batch files; the program chooses the address and opens the
browser. Until the Windows program has its own page, `START-HERE.bat` opens the
console workspace (see [Windows](WINDOWS_PACKAGE.md)).

Many Linux file managers open a `.sh` file in an editor. Use **Run as a
program** from its right-click menu, or run `./start-here.sh` in a terminal.

## What is inside

- `bin/`: `lumabri`, `tracker`, `maintainer`, `segment_node`, `segment_chat`,
  and on Linux and macOS `swarm_probe`, `lumabri-agent` and `lumabri-fetch`.
- `lib/lumabri/`: the weight loader library; on macOS also `libomp.dylib`.
- `licenses/`: Lumabri and Colibri licences and notices, plus the notice of
  the runtime each platform carries (OpenMP on macOS, the GCC runtime on
  Linux, the MinGW runtime on Windows).
- `manifest.json`: source commits, build system, dependencies and the hash of
  every file. Linux and macOS also carry `SHA256SUMS`; Windows carries
  `Verify-Lumabri.ps1`.

`lumabri-fetch`, the model download helper, is the only program that uses TLS:

- **Linux:** it uses the computer's own `libcurl.so.4`, so certificates and
  security fixes come from the distribution. Desktop installations normally
  have it. If the page says the download helper cannot run, install it
  (Debian, Ubuntu, Raspberry Pi OS: `sudo apt install libcurl4`) and reload.
  Everything else works without it. The archives are built and started on
  Ubuntu; other distributions ship `libcurl.so.4` in their own curl package
  and have not been tried.
  OpenMP is linked into the Segment programs; no compiler runtime is needed.
- **macOS:** it uses the system libcurl, which is always present.
- **Windows:** not available; copy a model folder or use another computer of
  the household to download.

## Check a download

Every release lists the SHA-256 of each archive in `SHA256SUMS`. In the folder
that holds the archive and `SHA256SUMS`:

```sh
sha256sum -c --ignore-missing SHA256SUMS      # Linux
shasum -a 256 -c --ignore-missing SHA256SUMS  # macOS
```

```powershell
Get-FileHash .\lumabri-<version>-windows-x86_64.zip -Algorithm SHA256   # compare with SHA256SUMS
```

After unpacking, `sha256sum -c SHA256SUMS` (Linux), `shasum -a 256 -c
SHA256SUMS` (macOS) or `./Verify-Lumabri.ps1` (Windows) checks every file
without running any of them. A matching hash shows that the download is
complete and is the file the release lists. It is not a publisher signature.

## Unsigned downloads

The archives are **not code-signed or notarized**. The system will ask before
running them. Check the hash first, then:

- **Windows:** SmartScreen shows "Windows protected your PC". Choose **More
  info**, then **Run anyway**. Do not disable Defender.
- **macOS:** the first double-click is refused. Open **System Settings →
  Privacy & Security**, find the blocked item and choose **Open Anyway**; macOS
  may ask again for each program in `bin`. See [macOS](MACOS_PACKAGE.md). No
  step here removes quarantine or disables Gatekeeper.
- **Linux:** no prompt; the file manager may need **Run as a program**.

## How they are built

`release.yml` runs on a `v*` tag or by hand. For each row it builds natively on
a GitHub-hosted runner from the pinned engine commit, assembles the folder with
`tools/package_household.py`, checks it with `--verify`, packs it with
`tools/release_archive.py`, then unpacks the archive on that runner and runs
`tests/integration/release_archive_smoke_test.py`: start on a private port,
load the page, sign in with the printed link, confirm that a first model is
offered, `service stop`. A tag attaches the five archives and `SHA256SUMS` to a
**draft** release; a person reviews and publishes it. Pull requests run the
same path for Linux x86-64 (`release-archive` job).

To build one row by hand on its own platform (Linux shown):

```sh
make household swarm_probe ENGINE=../colibri/c OMP_LIBS='-Wl,-Bstatic -lgomp -Wl,-Bdynamic'
python3 tools/package_household.py --colibri-root ../colibri --static-openmp --output build/release-candidate
python3 tools/release_archive.py --candidate build/release-candidate --platform linux-x86_64 --version 1.0.0 --output-dir build/release
python3 tests/integration/release_archive_smoke_test.py --archive build/release/lumabri-1.0.0-linux-x86_64.tar.gz
```

GPU backends are not built. When one exists it becomes a further row named
after its backend (`linux-x86_64-<backend>`) in `release_archive.py` and in the
workflow matrix; the CPU rows stay as they are.

The smoke test proves that an archive starts on its build runner's system. It
is not the clean-machine and physical LAN acceptance in the
[release checklist](LAN_RELEASE_CHECKLIST.md).
