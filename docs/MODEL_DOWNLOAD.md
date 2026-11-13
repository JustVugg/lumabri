# Downloading a model

Lumabri can fetch a model from the internet when you ask for it. It never
downloads anything on its own: not at start, not when the page opens, not
when a model is missing. Preparing and loading a downloaded model remain the
separate, approved steps described in
[Model preparation](MODEL_PREPARATION_API.md).

## From the page

When this computer has no model and your sign-in has management permission,
the chat view of the local page shows a **Get a model** card. It has three
faces:

- **A model that fits.** Its name, download size, the memory it needs, its
  licence, where it is published and the free space in the models folder,
  with a **Download** button. Progress, speed and a **Cancel download**
  button replace the button while it runs.
- **A model that is too large.** The same facts, greyed, and the sentence
  "Needs about N GB of memory across your computers. Add a computer to use
  it." Nothing can be started.
- **No downloadable model.** How to bring your own: copy a model folder or a
  GGUF file into the models folder.

After a download the model appears under **Computers & models → Prepare
models**. Nothing is prepared or loaded automatically.

The card is shown only by the page served on `127.0.0.1`. Behind the private
TLS proxy (`lumabri api start --https-origin …`) the endpoint answers
`403 downloads_local_gateway_only`.

## From a terminal

```sh
lumabri fetch list            # what is offered, what fits, what is here
lumabri fetch get ID          # download one model; resumes an unfinished one
lumabri fetch status          # progress of a running or interrupted download
lumabri fetch cancel          # stop and remove what was fetched
```

`lumabri fetch get` needs the household to be running (`lumabri start`),
because the memory the computers offer decides what may be fetched.
Ctrl-C stops a download and keeps what was fetched; the next `get` resumes.
`lumabri service stop` does the same to a download started from the page.

## What fits

Lumabri keeps every weight in memory and never reads weights from disk during
inference. A model is therefore offered only when the memory offered by the
computers of the household (the sum of the `ram_offered_bytes` that
`/api/v1/workspace` reports) is at least the model's resident weights plus
15 %. Before a model is on disk its exact plan cannot be computed, so this is
an **estimate**, and the page says so. The real placement is still computed
and reviewed under Prepare models. An entry that does not pass this test is
shown greyed in the page and refused by the API and by `lumabri fetch get`
(`model_too_large_for_household`). When the offered memory cannot be read,
nothing is offered (`household_memory_unknown`).

The resident weights are the entry's `resident_gb`, or its download size when
that field is absent.

## Safety rules

- **HTTPS only**, with certificate and host name verification. There is no
  option, variable or catalogue field that turns verification off. Plain HTTP
  is accepted only for `127.0.0.1` and `[::1]`, which is how the tests serve
  files.
- **Redirects** are followed by hand, at most five, each checked like a first
  address. A repository's files redirect to content servers on other HTTPS
  hosts; that is allowed. A redirect from HTTPS to plain HTTP is refused.
- **No credentials** are sent. Only public repositories can be fetched.
- **One snapshot.** A branch name is resolved to a commit once, at the start,
  and written to `.lumabri-download-<id>.revision` in the models folder. A
  resumed download continues that commit even if the branch has moved.
- **Checked before visible.** Files are collected in the hidden folder
  `.lumabri-download-<id>` inside the models folder, which the model
  catalogue does not look at. Each file is first written as
  `<name>.lumabri-part` and takes its name only when its size and hash match.
  The folder is renamed to `<models>/<id>`, or named GGUF files are placed in
  the models folder, only when every file is there. A failed, cancelled or
  wrong download never appears in the catalogue. An existing model with the
  same name is never replaced.
- **Hashes.** Large files (Git LFS) are checked against the SHA-256 the
  repository lists. Small files that the repository stores in git itself have
  no SHA-256 in the listing; they are checked against git's blob SHA-1 from
  the same listing, and by their size. SHA-1 detects damage and a wrong file
  but is not collision resistant; for those small files the protection is
  therefore the verified TLS connection to the pinned commit. Files named in
  the catalogue are checked against the SHA-256 written in the catalogue.
- **Paths.** A listed path with `..`, an absolute path, a backslash, a drive
  letter or a control character is refused before anything is fetched. Every
  folder is opened without following links, so a planted link cannot move a
  file outside the models folder.
- **Disk space.** The total of the listing, minus what is already there, plus
  2 GB must be free in the models folder before the first file is fetched.
- **One download at a time** per computer, held by a lock in
  `~/.lumabri/downloads`. Cancel removes what was fetched; an interruption
  (connection lost, Ctrl-C, service stop, power loss) keeps it, and the next
  start resumes with HTTP `Range` requests.

## The catalogue of downloadable models

The built-in list is `src/planner/download_catalogue.json`, compiled into
`lumabri`. Its fields mirror the engine's setup catalogue (`id`, `family`,
`name`, `repo`, optional `revision`, `disk_gb`, `ram_min_gb`, `ram_good_gb`,
`summary`), with these additions:

| Field | Meaning |
| --- | --- |
| `files` | Optional. `[{"path", "size", "sha256"}]`: fetch only these GGUF files, for a repository that holds many variants. No file list is requested from the server; sizes and SHA-256 come from the catalogue. The files are placed in the models folder itself, where raw GGUF files are found. `disk_gb` is then not needed. |
| `resident_gb` | Optional. Weights kept in memory, when that differs from the download size. |
| `licence`, `licence_url` | Optional. Shown on the card; the link must be HTTPS. Only state a licence that the publisher or the engine documentation states. |
| `unavailable_reason` | Optional. The entry cannot be downloaded yet; it is never offered and needs no `repo`. |
| `endpoint` (top level) | Optional. The hub's address, default `https://huggingface.co`. |

Sizes are GB of 10^9 bytes, as the publishers state them. `revision` is a
branch name or, better, a 40-character commit.

Without `files`, the whole repository at that commit becomes the folder
`<models>/<id>`.

The owner of the computer can replace the built-in list with
`~/.lumabri/download-catalogue.json` (a regular file owned by them and not
writable by group or others; otherwise it is refused, not skipped), or name a
file for one command with `lumabri fetch list|get --catalogue FILE`. An HTTP
caller can only name an `id` from the list in use: never an address, a file
name or a folder.

### Built-in entries

| id | What | Download | Memory estimate |
| --- | --- | --- | --- |
| `olmoe-1b-7b-instruct` | The starter: one native GGUF file (Q8_0) named with its size and SHA-256, at a pinned commit | 7.4 GB | about 8.5 GB |
| `qwen36-35b` | The second step: a converted container folder, branch `main` resolved at download time | 23.1 GB | about 27 GB |
| `qwen38-flash-next` | Official FP8 checkpoint at a pinned commit | 185.6 GB | about 213 GB |

The page offers the first entry that fits and lists the others, greyed when
the household cannot hold them.

### Adding or checking an entry

1. Choose a repository whose files Lumabri runs as they are: a converted
   Colibri container directory (see [Qwen architectures](QWEN_ARCHITECTURES.md))
   or a native GGUF file (see [GGUF](GGUF.md)).
2. Pin the commit. On the repository's "Files and versions" page, copy the
   full 40-character commit id into `revision`.
3. For one GGUF out of many, add it to `files` with its exact byte size and
   SHA-256. Both are shown on the file's page ("Git LFS details"), or compute
   them from a copy you trust:

   ```sh
   wc -c < model.gguf
   sha256sum model.gguf        # macOS: shasum -a 256 model.gguf
   ```

4. Check that the file is reachable without signing in at
   `https://huggingface.co/<repo>/resolve/<commit>/<path>`.
5. Rebuild (`make lumabri`), or put the entry in
   `~/.lumabri/download-catalogue.json` first, and run
   `lumabri fetch get ID` once on a real connection.
6. Confirm that the model appears under Prepare models and loads, before
   shipping the entry.

## What the downloader expects from the hub

The integration test imitates these three requests on loopback. The answers
of the real service were not exercised by the test suite.

| Request | Fields relied upon |
| --- | --- |
| `GET <endpoint>/api/models/<repo>/revision/<revision>` (only for a branch name) | JSON object with `sha`: the 40-character commit. Other members are ignored. |
| `GET <endpoint>/api/models/<repo>/tree/<commit>?recursive=true` (only without `files`) | JSON array. For each element with `type` equal to `"file"`: `path`, `size` (bytes), and either `lfs.oid` (SHA-256, 64 hex characters) or `oid` (git blob SHA-1, 40 hex characters). Other elements and members are ignored. A further page is announced by a `Link: <address>; rel="next"` header; it is accepted only from the same server. |
| `GET <endpoint>/<repo>/resolve/<commit>/<path>` | The file, directly or after redirects to HTTPS hosts. `200` for a whole file. For a request with `Range: bytes=N-`: `206` with `Content-Range: bytes N-…`, or `200`/`416`, after which the file is fetched again from the start. A `Content-Length`, when sent, must agree with the listed size. |

## Programs and files

- `lumabri-fetch` is the helper that does the transfer. It is the only
  household program that links libcurl (and through it the system TLS
  library), so that `lumabri`, the tracker and the workers keep no TLS
  dependency. It ships beside `lumabri` in the Linux and macOS packages. If
  it is missing or libcurl is not installed, everything else works and the
  page says that the download helper is unavailable.
- Build: `make household` builds it and needs the libcurl development files
  (Debian/Ubuntu: `sudo apt-get install libcurl4-openssl-dev pkg-config`;
  macOS: part of the system SDK). `make household FETCH_BINARY=` builds a
  household without model download.
- `~/.lumabri/downloads/` (owner only): `lock`, the progress record `state`,
  the `cancel` and `pause` request markers and `fetch.log` of the last
  download started from the page.
- The native Windows build has no web gateway and does not ship the helper.
  The address, name and catalogue rules in `src/runtime/lumabri_download.h`
  are plain C. A Windows helper would still need: the private state
  directory, lock and atomic record on the Win32 file API (the code uses
  `openat`, `flock`, `renameat` and `linkat`), directory handles that do not
  follow reparse points, a free-space query, a libcurl built for Windows with
  the system certificate store, and a way to start the helper detached.

## API

`/api/v1/downloads` on the loopback gateway. Management permission is
required for every method (`401` without a valid token, `403
management_permission_required` otherwise).

- `GET` returns the models folder, its free space, the memory the household
  offers, and for each downloadable model its facts, `memory_needed_bytes`,
  `fits` (`true`, `false`, or `null` when unknown) and `state`: `absent`,
  `downloading` (with `bytes`, `size_bytes`, `speed_bytes_per_second`),
  `interrupted`, `failed` (with `error`), `present` or `conflict`.
- `POST {"action":"start","id":"…"}` starts or resumes one download
  (`202`). `409` when the model does not fit, is already present, another
  download is running or disk space is short; `404` for an unknown id.
- `POST {"action":"cancel","id":"…"}` stops it and removes what was fetched.

## Tests

```sh
make lumabri lumabri-fetch household ENGINE=/path/to/colibri/c
python3 tests/integration/model_download_test.py
python3 tests/integration/web_download_test.py   # needs Playwright Chromium
```

The test starts a small HTTP server on loopback that imitates the hub and
covers: a whole snapshot with pagination and redirects; named files; SHA-256,
git hash and size mismatches; a connection broken mid-file and resumed with
`Range`; a killed helper resumed at the same commit after the branch moved; a
server without `Range`; cancel, pause and signals; the redirect limit; plain
HTTP to another host; path traversal and planted links; insufficient disk
space; a self-signed HTTPS certificate being refused; the three permission
cases; the memory gate; and stopping the services during a download.

The browser test drives the real compiled page against the real gateway: the
card for a model that fits, progress, cancel, completion, a model that is too
large, no model at all, and sign-out.
