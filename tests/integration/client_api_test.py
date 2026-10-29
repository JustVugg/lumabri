"""Client protocol acceptance against actual resident OLMoE, no mock generation.
Called by the existing two-model/user/service acceptance test. SDK checks are
explicitly enabled in CI/developer venvs; HTTP contract checks always run.
"""
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
import time


def verify_client_protocols(port, alice, bob, first, second):
    def request(path, token=alice, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        auth = {"Authorization": "Bearer " + token} if token else {}
        if body is not None:
            auth["Content-Type"] = "application/json"
        auth.update(headers or {})
        conn.request("GET" if body is None else "POST", path, None if body is None else json.dumps(body), auth)
        response = conn.getresponse()
        result = response.status, response.read()
        conn.close()
        return result

    def json_request(path, **kwargs):
        status, data = request(path, **kwargs)
        return status, json.loads(data)

    openai = "/v1/chat/completions"
    anthropic = "/v1/messages"
    version = {"anthropic-version": "2023-06-01"}
    messages = [{"role": "user", "content": "hi"}]
    base = {"model": first, "messages": messages, "max_tokens": 8, "temperature": 0}
    initial_history = {}
    for token, model in ((alice, first), (bob, second)):
        status, history = json_request("/api/v1/conversations", token=token)
        assert status == 200
        initial_history[token] = history
        status, answer = json_request("/v1/models", token=token)
        assert status == 200 and answer["object"] == "list", answer
        assert [m["id"] for m in answer["data"]] == [model], answer
    status, answer = json_request(openai, token="invalid", body=base)
    assert status == 401 and isinstance(answer["error"], dict), answer
    status, answer = json_request(anthropic, token=None, body=base, headers={**version, "x-api-key": "invalid"})
    assert status == 401 and answer["type"] == "error", answer
    assert request(anthropic, body=base, headers={**version, "x-api-key": alice})[0] == 400
    assert request(anthropic, body=base)[0] == 400
    assert request(anthropic, body=base, headers={**version, "anthropic-beta": "unimplemented"})[0] == 400
    for path, headers in ((openai, {}), (anthropic, version)):
        status, answer = json_request(path, token=bob, body=base, headers=headers)
        assert status == 403 and isinstance(answer["error"], dict)
        for extra in ({"tools": []}, {"thinking": {"type": "enabled"}}, {"unknown": True}):
            assert request(path, body={**base, **extra}, headers=headers)[0] == 400
        assert request(path, body=base, headers={**headers, "Origin": "https://untrusted.invalid"})[0] == 403

    status, full = json_request(openai, body=base)
    assert status == 200 and full["object"] == "chat.completion", full
    text = full["choices"][0]["message"]["content"]
    assert full["choices"][0]["finish_reason"] == "length" and full["usage"]["completion_tokens"] == 8, full
    prompt_count = full["usage"]["prompt_tokens"]
    assert prompt_count > 0 and full["usage"]["total_tokens"] == prompt_count + 8
    status, data = request(openai, body={**base, "stream": True, "stream_options": {"include_usage": True}})
    assert status == 200
    frames = [line[6:] for line in data.decode().splitlines() if line.startswith("data: ")]
    assert frames.pop() == "[DONE]"
    chunks = [json.loads(f) for f in frames]
    assert chunks[-1]["choices"] == [] and chunks[-1]["usage"] == full["usage"]
    assert len({c["id"] for c in chunks}) == 1 and len({c["created"] for c in chunks}) == 1
    assert "".join(c["choices"][0]["delta"].get("content", "") for c in chunks if c["choices"]) == text
    assert chunks[-2]["choices"][0]["finish_reason"] == "length"

    status, full_a = json_request(anthropic, token=None, body=base, headers={**version, "x-api-key": alice})
    assert status == 200 and full_a["type"] == "message" and full_a["content"][0]["text"] == text, full_a
    assert full_a["usage"] == {"input_tokens": prompt_count, "output_tokens": 8}
    status, data = request(anthropic, body={**base, "stream": True}, headers=version)
    assert status == 200
    events = [json.loads(line[6:]) for line in data.decode().splitlines() if line.startswith("data: ")]
    kinds = [e["type"] for e in events]
    assert kinds[:2] == ["message_start", "content_block_start"]
    assert kinds[-3:] == ["content_block_stop", "message_delta", "message_stop"]
    assert events[0]["message"]["usage"]["input_tokens"] == prompt_count
    assert events[-2]["usage"]["output_tokens"] == 8 and events[-2]["delta"]["stop_reason"] == "max_tokens"
    assert "".join(e["delta"]["text"] for e in events if e["type"] == "content_block_delta") == text

    # System roles and multiple turns take exactly the same template path.
    turns = [{"role": "user", "content": "hello"}, {"role": "assistant", "content": "hello"},
             {"role": "user", "content": [{"type": "text", "text": "hi"}]}]
    status, sys_o = json_request(openai, body={**base, "messages": [{"role": "system", "content": "Be brief."}, *turns]})
    assert status == 200, sys_o
    status, sys_a = json_request(anthropic, body={**base, "system": [{"type": "text", "text": "Be brief."}], "messages": turns}, headers=version)
    assert status == 200, sys_a
    assert sys_o["choices"][0]["message"]["content"] == sys_a["content"][0]["text"]
    assert sys_o["usage"]["prompt_tokens"] == sys_a["usage"]["input_tokens"] > prompt_count

    # The second model's single slot is deliberately held by its TUI here.
    # Compatibility endpoints must respect that reservation, not bypass it.
    status, busy = json_request(anthropic, token=bob, body={**base, "model": second}, headers=version)
    assert status == 429 and busy["error"]["type"] == "rate_limit_error", busy
    # Cancellation closes only this HTTP request and releases its slot. The
    # next normal request must succeed after bounded engine cleanup.
    for path, headers in ((openai, {}), (anthropic, version)):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        conn.request("POST", path, json.dumps({**base, "stream": True}),
                     {"Authorization": "Bearer " + alice, "Content-Type": "application/json", **headers})
        response = conn.getresponse(); assert response.status == 200
        response.close(); conn.close()
        deadline = time.monotonic() + 10
        while True:
            status, data = request(path, body=base, headers=headers)
            if status == 200:
                break
            assert status == 429 and time.monotonic() < deadline, (status, data)
            time.sleep(.1)

    # API calls preserve each user's history, including conversations already
    # created by the real browser test in the combined acceptance flow.
    for token, history in initial_history.items():
        assert json_request("/api/v1/conversations", token=token) == (200, history)
    if os.environ.get("LUMABRI_TEST_CLIENT_SDKS") == "1":
        import openai as oa
        import anthropic as an
        with oa.OpenAI(base_url=f"http://127.0.0.1:{port}/v1", api_key=alice, max_retries=0, timeout=30) as client:
            assert [m.id for m in client.models.list()] == [first]
            result = client.chat.completions.create(**base)
            assert result.choices[0].message.content == text
            with client.chat.completions.create(**base, stream=True, stream_options={"include_usage": True}) as stream:
                chunks = list(stream)
            assert chunks[-1].usage.completion_tokens == 8
            assert "".join(c.choices[0].delta.content or "" for c in chunks if c.choices) == text
        with an.Anthropic(base_url=f"http://127.0.0.1:{port}", api_key=alice, max_retries=0, timeout=30,
                          default_headers={"anthropic-version": "2023-06-01"}) as client:
            # SDK 2.x removed typed sampling arguments; its supported extra_body
            # path still transmits the 2023 text protocol's optional sampling.
            anthropic_args = {k: v for k, v in base.items() if k != "temperature"}
            result = client.messages.create(**anthropic_args, extra_body={"temperature": 0})
            assert result.content[0].text == text and result.usage.input_tokens == prompt_count
            with client.messages.stream(**anthropic_args, extra_body={"temperature": 0}) as stream:
                assert "".join(stream.text_stream) == text
                result = stream.get_final_message()
            assert result.usage.output_tokens == 8 and result.stop_reason == "max_tokens"
        with an.Anthropic(base_url=f"http://127.0.0.1:{port}", api_key=alice, max_retries=0, timeout=30) as client:
            result = client.messages.create(**anthropic_args)
            assert result.usage.input_tokens == prompt_count and 1 <= result.usage.output_tokens <= 8
            assert result.stop_reason in ("max_tokens", "end_turn")
            with client.messages.stream(**anthropic_args) as stream:
                modern_text = "".join(stream.text_stream)
                result = stream.get_final_message()
            assert result.content[0].text == modern_text
            assert 1 <= result.usage.output_tokens <= 8 and result.stop_reason in ("max_tokens", "end_turn")
        assert request(anthropic, body=base, headers={"anthropic-version": "2026-10-21"})[0] == 400
        print(f"OFFICIAL CLIENT SDKS: PASS (openai {oa.__version__}, anthropic {an.__version__}; local inference)", flush=True)
    print("CLIENT PROTOCOLS: PASS (real resident models, both dialects, isolation, streaming, usage, cancellation)", flush=True)


def verify_client_concurrency(port, tokens, models):
    import threading
    barrier = threading.Barrier(2)
    def turn(index):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        try:
            conn.connect(); barrier.wait(timeout=10)
            path = "/v1/messages" if index else "/v1/chat/completions"
            conn.request("POST", path, json.dumps({"model": models[index], "max_tokens": 8,
                "temperature": 0, "messages": [{"role": "user", "content": "hi"}]}),
                {"Authorization": "Bearer " + tokens[index], "Content-Type": "application/json",
                 "anthropic-version": "2023-06-01"})
            response = conn.getresponse(); data = json.loads(response.read())
            assert response.status == 200 and data["model"] == models[index], (response.status, data)
            assert data["usage"]["output_tokens" if index else "completion_tokens"] == 8
        finally:
            conn.close()
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(turn, (0, 1)))
    print("CLIENT CONCURRENCY: PASS (two protocols, separate users/models, shared admission)", flush=True)
