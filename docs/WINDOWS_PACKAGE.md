# Lumabri on Windows

Extract the entire ZIP into a new folder and open **Lumabri.cmd**. Keep its
`bin` folder alongside it. You do not need WSL, Python or a compiler.
This x86-64 package requires Windows 10 version 1903 or newer.

**START-HERE.bat** is the one-step start. It runs `lumabri start`: it creates
this computer's household if there is none, starts sharing, starts the local
web page and opens it in your default browser with a private sign-in link.
The page is served on `127.0.0.1` only. `START-HERE.bat --no-browser` prints
the link instead of opening it, `--tui` opens the console workspace, `--port N`
chooses the page port and `--ram-gb N` the memory a new household shares.
Everything keeps running after the window closes; `Lumabri.cmd service stop`
stops it.

The steps below describe the console workspace. The page offers the same
household: prepare a model under **Computers & models**, approve the request
for this computer on the page, then chat.

1. Choose **Create a household** on one computer. On the others, choose
   **Join a household** and use the displayed address and household key.
2. Open **Share resources** on the computers you want to contribute. Each
   computer approves its own allocation before loading model weights.
3. On the requesting computer, set the checkpoint folder under **Settings**.
   Use a local drive. Open **Explore models**, select computers, then review
   and request a model that fits their combined resources.
4. Accept the requests. The chat opens after the full chain is ready in memory.
   **Resident models** reopens prepared models without downloading them again.

Closing the workspace leaves approved models running in the background.
**Stop local services** releases this computer's models. Settings and identities
are stored privately under `%LOCALAPPDATA%\Lumabri`, outside the package.

The local Windows engine currently runs **OLMoE on CPU**. Other compatible
machines can serve models through the authenticated household protocol; their
advertised backend and memory limits determine what can run. This package does
not include GPU execution.

## The web page on Windows

`lumabri start` and `lumabri api start` serve the same compiled page as on
Linux and macOS, with the same bearer tokens and the same operator and
management permissions. Credentials, capabilities, the saved local sign-in
token and chat history live under `%LOCALAPPDATA%\Lumabri\api-access`. Each
of those files and folders is owned by your Windows account with a protected
access list that names only your account and the system; a record that anyone
else could read is refused, not used.

Working on Windows: sign-in, chat with a prepared model, private history, the
model catalogue, the computers view, preparing a model that is already in the
models folder, and approving or declining a request for this computer.

Not available on Windows yet. The page lists these after sign-in and each
request answers `not_available_on_windows_yet`:

- downloading a model from the page or with `lumabri fetch`: copy the model
  folder into the models folder shown by `lumabri start`;
- capacity measurement, portfolio policy and replica (elastic) policy;
- drain, resume and release of an allocation, and routing details;
- managed model routes with several replicas, and recovering a reply on
  another replica: a Windows gateway makes one attempt on the saved plan;
- demand counters and the API console;
- OpenAI and Anthropic compatible clients (`/v1/...`) and `--https-origin`;
- standing repeat-load consent and automatic acceptance for the household:
  each request is accepted or declined;
- adding a computer with a one-time code: a Windows computer joins with the
  full household key (see docs/PAIRING.md).

A model prepared from the page is added to the chat list of the sign-in that
asked for it when that sign-in next looks at the operation, or by the next
`lumabri start`. Only the latest preparation keeps its outcome; an older one
is shown as no longer recorded. A Linux or macOS gateway in the same household
still provides everything in the list above.

## Check the download and runtime

Use the package from the project's release or CI artifacts. Compare the archive
SHA-256 with the value published for that exact artifact. Internal manifest
hashes detect mixed or changed files; they do not authenticate the publisher.

From PowerShell inside the extracted folder:

```powershell
./Verify-Lumabri.ps1
./Lumabri.cmd doctor --household --json
```

The verifier checks the complete allowlist and every file hash. The doctor
starts bounded runtime capability queries without loading weights or sharing
resources. Neither check is a performance benchmark.

This candidate is not Authenticode-signed. If Windows blocks it, inspect its
source and the security message; do not disable Defender or global execution
policy. An organization can apply its own signing and application policy.

## Update or return to the previous version

Keep the previous complete package folder until the new version is verified.
Do not overwrite executables inside a running package.

1. Finish active chats and, from the **old** package, run:

   ```powershell
   ./Lumabri.cmd service stop
   ./Lumabri.cmd service status --json
   ```

2. Check that services stopped. Extract and verify the new complete package in
   a separate folder. Run its doctor, then open its `Lumabri.cmd`.
3. Household settings and identities remain. Start sharing again and prepare
   your models with fresh approvals. A saved plan does not authorize replaying
   an old allocation.
4. To roll back, stop services from the new package and reopen the previous
   complete folder. Use only versions covered by the release's compatibility
   evidence; arbitrary older versions may not understand newer state.

Updating deliberately unloads models. It does not silently migrate live
conversations or copy credentials into the package. A missing or damaged
runtime should be replaced as a whole package, never one executable at a time.
