# HTTPS access to the resident service

The default `lumabri api start` remains local to `127.0.0.1`. For access from
other authorized computers, explicitly select a single HTTPS origin and use
a TLS reverse proxy on the **same machine and OS account**. Lumabri does not
implement TLS, acquire certificates, open a firewall or provision a server.
The same API serves browser chat, private history and operator controls.
The same endpoint also serves the [OpenAI/Anthropic text client protocols](CLIENT_APIS.md).

## Trust boundary

In HTTPS mode Lumabri opens **no TCP API listener**. Its upstream is
`$HOME/.lumabri/api-access/proxy.sock`, under the canonical home path reported
in `lumabri service status --json`. The directory is private (0700), the socket
is owner-only (0600), and connections require matching kernel peer credentials.
The proxy is trusted: it terminates TLS and sees credentials and conversations,
just like the service operator. This is not end-to-end encryption from them.

Bearer credentials, individual model grants and separate operator/management
permissions remain mandatory. The proxy must preserve `Authorization`, `Host`
and `Origin`; it must **not** inject a shared credential or rewrite an arbitrary
origin into the authorized one. Lumabri matches the configured authority and,
when present, the full Origin exactly. `Forwarded` and `X-Forwarded-*` cannot
grant access. There is no wildcard origin, permissive CORS or cookie authority.

## Configure

First prepare models and create per-user grants as described in
[the API guide](RESIDENT_API.md#operator-setup). Then, as the same unprivileged
service account, stop an existing API and choose the intended public origin:

```sh
lumabri api stop
lumabri api start --https-origin https://chat.example.net:8443
lumabri service status --json
```

Use lowercase DNS names, no trailing slash, path, query or credentials. Omit
`:443` for the default HTTPS port; non-default ports must be explicit.
Bracketed IPv6 addresses are supported with exact authority matching.
`--https-origin` and `--port` are mutually exclusive. `api serve` accepts the
same flag for foreground diagnostics. A phase-2 API keeper proves that the
private upstream is listening, **not** that DNS, TLS or remote access works.

Copy [the NGINX template](../deploy/nginx-https.conf.example) to a private
operator-controlled location and replace every placeholder:

| Placeholder | Example / meaning |
| --- | --- |
| `@RUNTIME@` | `/srv/lumabri-proxy` — existing private writable directory |
| `@UPSTREAM@` | Exact `proxy.sock` path from service status |
| `@LISTEN@` | Approved server interface and port, such as `192.168.1.10:8443` |
| `@SERVER_NAME@` | `chat.example.net` |
| `@AUTHORITY@` | `chat.example.net:8443`, exactly matching the origin |
| `@CERTIFICATE@` | Absolute path to the server's certificate chain |
| `@KEY@` | Absolute path to its private key, readable only by this account |

Obtain a certificate trusted by the intended clients, with the matching DNS
name/IP and renewal managed by your deployment. Do not disable verification.
Do not copy the test certificate into production or make a private key public.
Start NGINX as the **same unprivileged account**, not the system-wide
`www-data` worker and not via `sudo`:

```sh
nginx -t -c /srv/lumabri-proxy/nginx.conf
nginx -c /srv/lumabri-proxy/nginx.conf
```

The supplied configuration stays in the foreground for supervision. It uses
TLS 1.2/1.3, bounded connections/request rate/body size, no access log, no proxy
cache, no response buffering and no upstream retry. Requests are buffered
before forwarding so the bounded HTTP/1.1 upstream receives Content-Length
framing. Responses stream immediately; a disconnected client closes its
upstream request. Do not enable retries for POSTs: inference or management
may already have started. Do not enable request/header logging containing
tokens or text. Review retained error logs as private operational data.

Open the configured HTTPS URL, enter your own API token and select a granted
resident model. For command-line clients, use the same URL, verified TLS and
`Authorization: Bearer ...`; never place the token in a query string.

## Lifecycle and deployment checks

- Repeating `api start` with the same tracker and origin reuses the keeper.
  Changing the origin or switching between TCP and Unix mode requires an
  explicit `api stop`. A manager restart leaves the API and allocations alive.
- API stop cancels its active requests but does not unload resident models.
  This is not a zero-downtime gateway upgrade. A crashed API can reclaim its
  stale socket; an active listener, unsafe file or symlink is not overwritten.
- Do not relax the socket's permissions to support another proxy account.
  A short canonical home path is required by Unix socket path limits (notably
  on macOS). A path-too-long failure has no TCP fallback.
- Verify unauthorized requests fail, wrong Host/Origin fail, each user sees
  only granted models/history, streamed output is not buffered, and Stop
  releases admission. Confirm the upstream has no exposed TCP port.
- Verify certificate renewal, actual DNS/firewall/routing and proxy restart
  on the target machines. Choose rate/connection limits for that deployment;
  NGINX defaults in this recipe are not a capacity promise or DDoS service.
  Consider HSTS only once the intended HTTPS origin is operational.
- Remote ingress does not authorize new donor machines, change residency,
  certify model throughput or implement Kubernetes/cloud provisioning.

## Reproducible local checks

The Unix lifecycle/authentication gate runs in ordinary Linux/macOS CI:

```sh
python3 tests/integration/api_proxy_test.py
```

With an already available NGINX executable and OpenSSL, test the exact template
against an ephemeral loopback listener and a temporary explicitly trusted
certificate. This test changes neither the machine's trust store nor its
system services:

```sh
python3 tests/integration/api_https_test.py --nginx /path/to/nginx
LUMABRI_RESIDENCY_POLICY=engine-locked \
python3 tests/integration/household_service_flow_test.py \
  --models-dir build/home-flow-models --joint complete --api-prepare \
  --donor-ram-gb 1.5 --sessions 2 --https-nginx /path/to/nginx
```

The second gate prepares two real reduced-OLMoE identities, verifies separate
user grants, concurrent streamed turns, client Stop, private history and API
restart without changing resident allocation ownership or reservations. The
report is `https-resident.json`; generated text and credentials are not logged.
This is local protocol/inference evidence, not an installed datacenter test.
