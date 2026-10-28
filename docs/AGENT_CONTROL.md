# External agents

`lumabri-agent` is a C MCP server over standard input/output. It connects to
the existing Lumabri API on `127.0.0.1`; it has no separate scheduler, allocation
store or inference engine. MCP protocol versions `2025-11-25` and `2025-06-18`
are supported. It exposes tools and five embedded management-contract resources.
The protocol and contracts are available without an AI subscription or SDK.

Start the ordinary API service first. Create a dedicated credential instead
of giving an agent the owner's shell or sharing a chat user's credential:

```sh
umask 077
lumabri api user-add observer > "$HOME/.lumabri/observer.token"
lumabri api operator-grant observer
lumabri-agent --token-file "$HOME/.lumabri/observer.token" --port 47380
```

The last command is normally launched by the MCP client, not typed into a
terminal for interactive chat. In that client's stdio-server configuration,
set the command to the installed `bin/lumabri-agent` and the arguments to
`["--token-file", "/absolute/path/observer.token", "--port", "47380"]`.
Use absolute paths. This documents the MCP process contract; client-specific
configuration formats may differ.

The default eight tools inspect computers, model visibility, catalogue,
demand, workload observations, policies and exact allocation status. They do
not expose conversation history, arbitrary file access, shell commands or
arbitrary HTTP endpoints. Missing measurements stay missing. The resource
`lumabri://docs/preparation`, for example, contains the actual preparation
contract built into the same executable.

## Delegate management explicitly

To delegate mutations, the owner must both grant API management permission
and add `--allow-management` to this MCP server's arguments:

```sh
lumabri api manage-grant observer
```

The additional tools cover durable preparation, drain/resume/retirement,
capacity check/select/apply, and bounded elastic/portfolio policy changes.
Arguments wrap the existing API body in `request`; allocation tools additionally
require `allocation`. The embedded resources describe exact fields, limits and
retry semantics. No independent body-to-shell translation is performed.

The service remains authoritative for selected computers, memory budgets,
donor permissions, review digests, operation IDs and instance/revision fences.
Management is separate from inference and private-history access. An agent
cannot approve another computer's resources, raise quotas, invent a matching
measurement, or create paid servers through this bridge. Existing donor repeat
authorization and policy limits still apply.

Before mutation, read the corresponding contract and observe current state.
Use the same immutable operation ID to inspect a lost preparation reply.
Re-read the exact instance and revision after a conflict. The bridge never
retries POSTs automatically. A timeout or disconnected reply is an **unknown
outcome**, not proof of rollback. Drain acknowledgement is not completion;
poll allocation status before assuming resources can be retired.

`lumabri api manage-revoke observer` removes mutation authority immediately
for later requests. The bridge re-reads the private token file for every call;
the API rechecks permissions. The token must be a regular owner-only file and
is never sent through MCP output or logged. Logs contain tool names and HTTP
outcomes, not request bodies or credentials.

## Boundaries and verification

This implementation uses local HTTP and MCP stdio, not a remotely exposed MCP
HTTP listener. A remote administrator runs it beside the authenticated gateway
or supplies their own protected transport to that host. No TLS verification
is disabled. Requests are bounded to 16 KiB, API replies to 1 MiB, and each
HTTP exchange to 15 seconds. Documentation resources are embedded at build time;
tools cannot read arbitrary local files. Standard output contains only JSON-RPC.

Short management requests are serialized. This version does not expose the
long-running capacity measurement stream as an MCP tool or claim asynchronous
MCP task/cancellation support. Cancelling an agent does not cancel an already
accepted durable preparation or stop active model sessions; use the service's
explicit operation controls. The inference service continues if the agent exits.

`make test-agent-control` checks initialization, discovery, framing, credential
permissions, refusal of redirects/retries, and live API grant revocation. The
two-model resident acceptance also exercises status, drain/resume, neighbour
isolation and stale-fence refusal through the shipped MCP executable before
running inference again. These tests prove the protocol and service boundary;
they do not measure a particular external agent's decision quality.
