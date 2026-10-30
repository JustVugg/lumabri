# Connect applications to Lumabri

Lumabri implements documented subsets of OpenAI Chat Completions, OpenAI
Responses and Anthropic Messages, including function calls and inline images
on the Qwen3.8 vision runtime. Inference runs on your approved Colibri cluster;
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

## Responses client

```python
with client.responses.create(
    model=os.environ["LUMABRI_MODEL"],
    input="Explain this briefly.",
    max_output_tokens=128,
    store=False,
    stream=True,
) as stream:
    for event in stream:
        if event.type == "response.output_text.delta":
            print(event.delta, end="", flush=True)
        elif event.type == "response.incomplete":
            print("\nOutput limit reached:", event.response.incomplete_details)
```

Responses accepts string input, message items, `function_call` and matching
`function_call_output` items. Send the conversation history with each request.
`instructions`, function `tools`, `tool_choice`, `parallel_tool_calls`,
`max_output_tokens`, sampling and streaming are supported. This endpoint does
not store responses: use `store=False`; `previous_response_id`, background
responses, retrieval/deletion and nonempty `include` are rejected. A token
limit emits `response.incomplete`, never a false `response.completed`.

## Function calls

Offer functions through OpenAI `tools`/`tool_choice`, Responses flat function
definitions, or Anthropic `tools`/`tool_choice`. Lumabri supplies the definitions
to the local model and returns its generated calls. Your application decides
which functions to execute and returns results with matching call IDs.

```python
messages = [{"role": "user", "content": "What is the temperature in Rome?"}]
reply = client.chat.completions.create(
    model=os.environ["LUMABRI_MODEL"], messages=messages, max_tokens=256,
    tools=[{"type": "function", "function": {
        "name": "get_weather", "description": "Read current weather for a city",
        "parameters": {"type": "object", "properties": {"city": {"type": "string"}},
                       "required": ["city"]},
    }}],
)
# Inspect reply.choices[0].message.tool_calls in your own application.
# Validate and authorize each operation before executing it.
```

Chat Completions returns `tool_calls` and `finish_reason: tool_calls`;
Anthropic returns `tool_use` blocks and `stop_reason: tool_use`; Responses
returns `function_call` output items. Their corresponding result formats are
accepted on the next turn. Unknown, duplicate or unmatched results are rejected.
There are at most 64 function definitions and 16 calls in a turn.

The generic function prompt does not add tool-selection ability to a model
that was not trained for it. Generated calls must name a declared function and
contain a complete JSON object. Required/named tool choice and disabled
parallel calls are enforced. Tool-enabled replies are buffered until validated,
then emitted in the client's full or streaming format; malformed/truncated
calls produce an error. `strict:true` is not supported, and applications must
validate argument values against their own schemas. Lumabri never executes
functions supplied by a chat client.

## Images

Use a checkpoint with actual Qwen3.8 visual tensors, not a text-only export.
Its visual tower and temporary work are included in the approved Edge budget;
images are decoded in memory and projected by Colibri before the same Segment
chain runs. Other adapters currently return `images_not_supported_by_model`.

```python
import base64
from pathlib import Path

encoded = base64.b64encode(Path("photo.png").read_bytes()).decode("ascii")
response = client.responses.create(
    model=os.environ["LUMABRI_MODEL"], store=False, max_output_tokens=128,
    input=[{"role": "user", "content": [
        {"type": "input_text", "text": "Describe this image."},
        {"type": "input_image", "image_url": "data:image/png;base64," + encoded},
    ]}],
)
print(response.output_text)
```

Chat Completions uses `image_url: {"url": "data:image/png;base64,..."}`;
Anthropic uses `{"type":"image","source":{"type":"base64",
"media_type":"image/png","data":"..."}}`. JPEG is also accepted. Only user
message content accepts images. `detail` may be omitted or `auto`.

Limits are explicit: four images, 768 KiB encoded bytes per image, 4 megapixels
per source image, 1 MiB for the entire JSON request, and at most 256 visual
tokens per image. Resize large photos before sending. The pinned Colibri
processor uses RGB, patch merging and normalization; it does not infer
unsupported resolution modes. There is no remote URL fetch, filesystem-path
access, video or image generation. Reusing the same image placeholder with a
different image resets the conversation's runtime state rather than reusing
incompatible KV. The client must resupply prior images when they are needed
in a later turn.

## Exact compatibility boundary

| Endpoint | Supported |
| --- | --- |
| `GET /v1/models` | OpenAI list shape; only this credential's authorized allocations and managed model IDs. `created: 0` means no creation timestamp is recorded. Listing does not assert that a replica is ready. |
| `POST /v1/chat/completions` | Text, inline images, function calls/results, full JSON response or streamed `chat.completion.chunk` / `[DONE]`; one choice; usage when requested. |
| `POST /v1/messages` | Text, inline images, tool use/results, full Message response or ordered Messages SSE events; `anthropic-version: 2023-06-01` or `2026-10-21`; no beta extensions. |
| `POST /v1/responses` | Stateless text/image/function input; full Response or Responses SSE lifecycle, usage and incomplete/error status. |
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

**Not implemented:** embeddings, audio/video, remote image URLs, file IDs,
server-executed tools, `strict:true` schema-constrained generation, assistant
prefills, JSON-schema text generation, stop strings, penalties,
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
The existing two-model resident service test also runs all three protocols against
real reduced OLMoE engines: equivalent greedy text/usage, model isolation,
concurrent different-model requests, cancellation and follow-up requests.
Set `LUMABRI_TEST_CLIENT_SDKS=1` with the official Python `openai` and `anthropic`
packages installed to exercise their full-response and streaming consumers.
No provider key or external inference service is used by these tests.

The vision gate compares preprocessing with the pinned Colibri reference and
visual rows with an independent Transformers model, then checks local/split
greedy inference and full/streamed image requests. These generated checkpoints
exercise arithmetic, transport and memory; they do not measure the visual
answer quality or function-selection quality of a trained model.
