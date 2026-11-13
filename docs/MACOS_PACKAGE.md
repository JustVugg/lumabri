# Lumabri on macOS

Download the complete candidate for your Mac: **Intel** or **Apple Silicon**.
Extract it into a new folder. Keep `Lumabri.command`, `bin` and `lib` together;
do not replace individual files inside an older installation. The multicore
package includes OpenMP and does not need Homebrew or Python on your computer.

## Verify before opening

Compare the archive's SHA-256 with the value attached to that exact CI artifact.
From Terminal inside the extracted folder, check the payload without running
any packaged executable:

```sh
shasum -a 256 -c SHA256SUMS
```

Every listed file must report `OK`. A failure means the package is incomplete
or changed: extract a fresh complete copy. These hashes detect mixed or damaged
files; they are not a publisher signature or a malware scan. The checksum list
covers the shipped payload; `manifest.json` records its build and full file list.

## First launch and macOS approval

This candidate **is not Developer ID signed or notarized**. macOS can block
`lumabri`, `segment_node`, `segment_chat`, `liblumabri.dylib` or bundled
`libomp.dylib` independently. Approving the launcher does not necessarily approve
every helper. No package command removes quarantine or disables Gatekeeper.

1. Open **Lumabri.command**. If macOS blocks a component, read its exact name.
2. If you trust this verified download, use Apple's **Open Anyway** action for
   that component in **System Settings → Privacy & Security**. On Monterey,
   the pane is **System Preferences → Security & Privacy → General**.
3. Retry the launch or runtime check. If another helper is blocked, review that
   helper separately. If macOS identifies actual malware or an organization
   disallows an exception, stop and follow that policy.

See [Apple's instructions](https://support.apple.com/102445). Ad-hoc signatures
and matching hashes do not provide notarization. A signed public distribution
requires the publisher's Developer ID and Apple's notarization service.

Before sharing resources, run this from the package folder:

```sh
./Lumabri.command doctor --household --json
```

It starts bounded runtime probes, loads no model and shares no resources. A
failed check identifies the helper to inspect; do not retry a model allocation
until its runtime is usable. It does not prove inference performance.

## Join and use your computers

To start on one Mac without typing commands, open **START-HERE.command**. It
creates this Mac's household if there is none, starts sharing and the local web
page, and opens the chat page in your browser. The page is served to this Mac
only. **Lumabri.command** still opens the terminal workspace described below.

Open **Lumabri.command**. Create a household on one computer and join it on the
others with its displayed address and key. Open **Share resources** on helpers.
On the requester, set the model folder in **Settings**, select the computers
and review a model plan. Each participating computer approves its own allocation.
Chat opens after the complete chain is prepared in memory.

Closing the terminal workspace leaves approved models running. Reopening
**Share resources** reconnects to that service. **Stop local services** unloads
this computer's models. Keep macOS's application firewall enabled and allow
the installed `bin/lumabri` and `bin/segment_node` when prompted for LAN access.
Application permission and Gatekeeper's download approval are separate checks.

## Update or roll back

Finish active chats. From the **old** package folder run:

```sh
./Lumabri.command service stop
./Lumabri.command service status --json
```

After services stop, verify and open the new package from its separate folder.
Settings and identities remain under `~/.lumabri`. Start sharing again and
prepare models with fresh approvals. An update unloads weights; it does not
silently migrate active conversations. Keep the previous complete folder for
the supported rollback pair; stop the new services before reopening it.

The package manifest records the minimum build target. Native CI and a passing
doctor are distinct from verification on your exact Mac and model.
