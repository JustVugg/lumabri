# Lumabri on Windows

Extract the entire ZIP into a new folder and open **Lumabri.cmd**. Keep its
`bin` folder alongside it. You do not need WSL, Python or a compiler.
This x86-64 package requires Windows 10 version 1903 or newer.
**START-HERE.bat** opens the same workspace. The Windows package has no web
server, so neither file opens a browser chat page on this computer.

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
not include GPU execution or a Windows HTTP/web API server. The Linux/macOS
gateway provides the web and application API for a mixed household.

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
