"""Real authenticated API -> resident vision Edge -> Segment inference."""
import base64
import http.client
import json
from pathlib import Path


def verify_images(port, token, model, images_dir):
    def post(path, body, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=90)
        connection.request("POST", path, json.dumps(body), {"Authorization": f"Bearer {token}",
            "Content-Type": "application/json", **(headers or {})})
        response = connection.getresponse(); data = response.read(); status = response.status
        connection.close()
        return status, data

    results = []
    for image in (0, 1, 0):  # Same placeholder tokens with different actual image rows.
        data = base64.b64encode((Path(images_dir) / f"image-{image}.png").read_bytes()).decode()
        url = "data:image/png;base64," + data
        requests = [
            ("/v1/chat/completions", {"model": model, "max_tokens": 4, "temperature": 0,
                "messages": [{"role": "user", "content": [{"type": "text", "text": "Describe."},
                    {"type": "image_url", "image_url": {"url": url}}]}]}, {}),
            ("/v1/messages", {"model": model, "max_tokens": 4, "temperature": 0,
                "messages": [{"role": "user", "content": [{"type": "text", "text": "Describe."},
                    {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": data}}]}]},
                {"anthropic-version": "2023-06-01"}),
            ("/v1/responses", {"model": model, "max_output_tokens": 4, "temperature": 0, "store": False,
                "input": [{"role": "user", "content": [{"type": "input_text", "text": "Describe."},
                    {"type": "input_image", "image_url": url}]}]}, {}),
        ]
        output = []; counts = []
        for path, body, headers in requests:
            status, raw = post(path, body, headers)
            assert status == 200, (path, status, raw)
            reply = json.loads(raw)
            if path.endswith("completions"):
                output.append(reply["choices"][0]["message"]["content"]); counts.append(reply["usage"]["prompt_tokens"])
            elif path.endswith("messages"):
                output.append(reply["content"][0]["text"]); counts.append(reply["usage"]["input_tokens"])
            else:
                output.append(reply["output"][0]["content"][0]["text"]); counts.append(reply["usage"]["input_tokens"])
            status, raw = post(path, {**body, "stream": True}, headers)
            assert status == 200, (path, status, raw)
            events = [json.loads(line[6:]) for line in raw.decode().splitlines() if line.startswith("data: ") and line[6:] != "[DONE]"]
            assert all(e.get("type") not in ("error", "response.failed") and not e.get("error") for e in events), events
            if path.endswith("completions"):
                streamed = "".join(e["choices"][0]["delta"].get("content", "") for e in events if e.get("choices"))
            elif path.endswith("messages"):
                streamed = "".join(e["delta"].get("text", "") for e in events if e["type"] == "content_block_delta")
            else:
                streamed = "".join(e["delta"] for e in events if e["type"] == "response.output_text.delta")
            assert streamed == output[-1], (path, streamed, output[-1])
        assert len(set(output)) == len(set(counts)) == 1 and counts[0] >= 64, (output, counts)
        results.append(output[0])
    assert results[0] == results[2], "prior image rows leaked into the next conversation"
    # Generation differences are model-dependent; numerical image sensitivity
    # is separately required by test_vision_segment against independent rows.
    print("VISION API: three protocols, real image prefill, streaming parity and image replacement PASS")
