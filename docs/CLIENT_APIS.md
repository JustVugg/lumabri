# Connect applications to Lumabri

Lumabri provides a **text chat subset** of the OpenAI Chat Completions and
Anthropic Messages protocols. Inference runs on your approved Colibri cluster;
no prompt, key or request is sent to either external provider. These adapters
share the existing gateway, model permissions, admission, cancellation and
replica recovery. There is no second inference service to deploy.

## Give an integrator three things

1. **The API URL.** Local default: `http://127.0.0.1:47380`. Remote clients need
   your configured [HTTPS endpoint](HTTPS_INGRESS.md).
2. **Their Lumabri API token**, delivered privately, never in a URL or committed
   configuration. This is not an OpenAI/Anthropic account credential.
3. **An authorized model ID**, from `GET /v1/models`. Managed model IDs remain
   stable when the approved replica set changes. Display names are not IDs.

The operator prepares a model, then creates a restricted application account:

```sh
lumabri api list
lumabri api user-add my-application
lumabri api grant my-application MODEL_ID
lumabri api start
```

`user-add` prints the token once. Keep it in the application's secret store.
A chat application does not need operator or management permission. Revoke it
with `lumabri api revoke my-application` when access should end.

Using that token, the application can retrieve a **secret-free connection
sheet** at `GET /api/v1/client-config`. It contains both SDK base URLs, the
Anthropic protocol version, the model-list URL and the supported scope. It
reports configuration, not a successful external DNS/TLS/connectivity test.

```sh
export LUMABRI_URL=http://127.0.0.1:47380
# Set LUMABRI_API_KEY privately, using the token printed by user-add.
curl --fail-with-body "$LUMABRI_URL/api/v1/client-config" \
  -H "Authorization: Bearer $LUMABRI_API_KEY"
curl --fail-with-body "$LUMABRI_URL/v1/models" \
  -H "Authorization: Bearer $LUMABRI_API_KEY"
```

## OpenAI client

```python
import os
from openai import OpenAI

client = OpenAI(
    base_url=os.environ["LUMABRI_URL"].rstrip("/") + "/v1",
    api_key=os.environ["LUMABRI_API_KEY"],
    max_retries=0,
)
with client.chat.completions.create(
    model=os.environ["LUMABRI_MODEL"],
    messages=[{"role": "user", "content": "Hello"}],
    max_tokens=128,
    stream=True,
) as stream:
    for chunk in stream:
        if chunk.choices:
            print(chunk.choices[0].delta.content or "", end="", flush=True)
```

## Anthropic client

```python
import os
from anthropic import Anthropic

client = Anthropic(
    base_url=os.environ["LUMABRI_URL"].rstrip("/"),
    api_key=os.environ["LUMABRI_API_KEY"],
    max_retries=0,
)
with client.messages.stream(
    model=os.environ["LUMABRI_MODEL"],
    max_tokens=128,
    messages=[{"role": "user", "content": "Hello"}],
) as stream:
    for text in stream.text_stream:
        print(text, end="", flush=True)
```

The SDKs are application dependencies, not Lumabri runtime dependencies.
[The runnable example](../examples/clients/chat.py) supports both clients.
Set `LUMABRI_MODEL` to an actual ID returned for your token. A model name such
as `gpt-*` or `claude-*` does not select a local model automatically.

## Exact compatibility boundary

| Endpoint | Supported |
| --- | --- |
| `GET /v1/models` | OpenAI list shape; only this credential's authorized allocations and managed model IDs. `created: 0` means no creation timestamp is recorded. Listing does not assert that a replica is ready. |
| `POST /v1/chat/completions` | Text, full JSON response or streamed `chat.completion.chunk` / `[DONE]`; one choice; usage when requested. |
| `POST /v1/messages` | Text, full Message response or ordered Messages SSE events; `anthropic-version: 2023-06-01` or `2026-10-21`; no beta extensions. |
| `GET /api/v1/client-config` | Authenticated connection sheet without credentials. |

Both chat endpoints accept `model`, `messages`, `max_tokens` and `stream`.
OpenAI and Anthropic version `2023-06-01` also accept `temperature` and `top_p`.
Version `2026-10-21` rejects sampling parameters, as does the current Anthropic API. Messages accept strings or arrays of
`{"type":"text","text":"..."}`. A conversation must begin and end with a
user turn after any system instructions. Consecutive same-role turns are
joined with two newlines. Limits: 130 input messages, 64 KiB decoded text per
merged turn/system instruction, 1 MiB HTTP body and at most 4096 generated
tokens, further restricted by the approved model/context contract.

OpenAI additionally supports leading `system`/`developer` instructions,
`max_completion_tokens` **instead of** `max_tokens`, `n: 1`, `store: false`,
32-bit nonnegative `seed`, and `stream_options.include_usage` (streaming only).
`include_obfuscation: false` is accepted; obfuscation is not implemented.
An omitted output limit defaults to 256 and is still checked against the
model's approved limit. Anthropic requires `max_tokens` and accepts a separate
`system` string or array of text blocks.

Sampling uses the existing engine codec. Nonzero temperature on a greedy-only
adapter, or a seed on a runtime without request-seed support, is rejected.
Temperature is 0–2 for OpenAI and 0–1 for Anthropic; `top_p` is greater than 0
and at most 1. OpenAI defaults to the existing household sampling policy (temperature 0.7,
top-p 0.95); Anthropic defaults to temperature 1 and top-p 0.95. Greedy-only
adapters use temperature 0 when no explicit sampling parameter is requested.
For SDK 2.x calls that need sampling parameters, select `2023-06-01` through
`default_headers` and pass sampling fields with `extra_body`.
System messages retain their family-specific system role. Model reasoning
text remains text; it is not converted into signed Anthropic thinking blocks.

**Not implemented:** OpenAI Responses, embeddings, audio/images, tools/function
calling, assistant prefills, JSON-schema generation, stop strings, penalties,
log probabilities, provider prompt caching and beta features. Unsupported
fields are rejected before model execution; they are never silently ignored.
This does not imply drop-in support for every agent or UI using those APIs.
Use [the separate MCP control interface](AGENT_CONTROL.md) for authorized
cluster administration.

Anthropic accepts `x-api-key` or Bearer authentication, never both in one
request. OpenAI uses Bearer. Host/Origin restrictions, individual model grants
and request limits apply to both. `429` denotes exhausted admission capacity;
clients should use bounded backoff, not blindly replay an interrupted POST.
Disconnect cancels only that request. A stream error has no success terminator.
Full-response requests return an error rather than a partial successful answer.

Token usage comes from the engine's tokenizer and sampled tokens (including
an end-of-sequence token). It is not billing data. Finish reasons come from
explicit Segment completion metadata; update the complete household package
before using these endpoints. Old engines lacking that metadata produce a
visible incomplete-response error. Recovery uses the existing verified-prefix
replay; final usage describes the successful attempt, not the extra recovery
compute. Chat history is client supplied; these calls do not implicitly save
conversations in the web history API.

## Kubernetes: who provides the address?

Lumabri owns the API implementation. Kubernetes routes connections to its
running gateway through a **Service**. Your infrastructure operator configures
DNS, trusted TLS and, where needed, a load balancer or ingress controller.
Nothing is submitted to the Kubernetes project or to an external AI provider.

1. Run Lumabri and the same-user TLS proxy using [the HTTPS recipe](HTTPS_INGRESS.md).
   Preserve the gateway's private state and Unix upstream in the deployment.
2. Label that gateway/proxy Pod `app: lumabri-api`. This is distinct from the
   worker Pods; worker ports are not client chat APIs.
3. Adapt [the Service example](../deploy/kubernetes/client-api.example.yaml)
   to the proxy's actual TLS listening port. For external access, use your
   installed load-balancer/ingress infrastructure. The example is ClusterIP
   only and creates no paid load balancer, Pods or VMs.
4. Configure `--https-origin` with the exact URL clients will use. Its DNS name
   must resolve to the chosen entry point and be covered by a trusted certificate.
   A Service DNS name is not automatically a valid TLS name or allowed Host.
5. Fetch `/api/v1/client-config` through that URL and test `/v1/models` with a
   restricted client token. Supply the URL/model through application configuration
   and the token through a Kubernetes Secret or your existing secret manager.

The proxy must retain `Authorization`, `x-api-key`, `anthropic-version`, `Host`
and `Origin`, stream responses without buffering, and close the upstream when
the client disconnects. The supplied NGINX recipe forwards these headers.
Do not inject a shared token or relax the owner-only upstream permissions.

## Verification

`make test-api` covers strict parsing, family system framing, sampling values,
UTF-8 split across engine frames, complete/error termination and token usage.
The existing two-model resident service test also runs both protocols against
real reduced OLMoE engines: equivalent greedy text/usage, model isolation,
concurrent different-model requests, cancellation and follow-up requests.
Set `LUMABRI_TEST_CLIENT_SDKS=1` with the official Python `openai` and `anthropic`
packages installed to exercise their full-response and streaming consumers.
No provider key or external inference service is used by these tests.
