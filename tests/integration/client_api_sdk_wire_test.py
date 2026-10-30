"""Consume production C wire fixtures with the official SDK accumulators.

The C contract test writes these fixtures through real socket pairs. This test
checks protocol interoperability, not model quality; resident engine acceptance
lives in client_api_test.py. No provider request is made.
"""
import json
from pathlib import Path
import sys

import anthropic
import httpx2 as httpx
import openai

MODEL = "01" * 32
ROOT = Path(sys.argv[1])


def transport(name, http=httpx):
    wire = (ROOT / name).read_bytes()
    if wire.startswith(b"HTTP/"):
        head, body = wire.split(b"\r\n\r\n", 1)
        assert b"200 OK" in head
        headers = {"content-type": "application/json"}
    else:
        body = wire
        headers = {"content-type": "text/event-stream"}

    def receive(request):
        assert request.url.host == "lumabri.test"
        assert json.loads(request.content)["model"] == MODEL
        return http.Response(200, headers=headers, content=body)

    return http.Client(transport=http.MockTransport(receive))


for streaming in (False, True):
    for eos in (False, True):
        with openai.OpenAI(api_key="local-contract-test", base_url="http://lumabri.test/v1",
                           http_client=transport(f"responses-text-{int(streaming)}-{int(eos)}.http")) as client:
            if streaming:
                with client.responses.stream(model=MODEL, input="hi", store=False) as stream:
                    events = list(stream)
                    # SDK 3.3.1's accumulator only records response.completed.
                    # A length-limited generation must still use the official
                    # response.incomplete event, never invent completion to
                    # satisfy that convenience method.
                    result = stream.get_final_response() if eos else events[-1].response
                assert [e.sequence_number for e in events] == list(range(len(events)))
            else:
                result = client.responses.create(model=MODEL, input="hi", store=False)
            assert result.output_text == "Hello", result
            assert result.status == ("completed" if eos else "incomplete")
            assert result.usage.total_tokens == 15
            assert result.incomplete_details is None if eos else result.incomplete_details.reason == "max_output_tokens"

    with openai.OpenAI(api_key="local-contract-test", base_url="http://lumabri.test/v1",
                       http_client=transport(f"tools-1-{int(streaming)}.http")) as client:
        if streaming:
            with client.chat.completions.stream(model=MODEL, messages=[{"role": "user", "content": "hi"}]) as stream:
                list(stream)
                result = stream.get_final_completion()
        else:
            result = client.chat.completions.create(model=MODEL, messages=[{"role": "user", "content": "hi"}])
        choice = result.choices[0]
        assert choice.finish_reason == "tool_calls"
        assert choice.message.content == "Checking."
        assert choice.message.tool_calls[0].function.name == "weather"
        assert json.loads(choice.message.tool_calls[0].function.arguments) == {"city": "Roma"}

    with anthropic.Anthropic(api_key="local-contract-test", base_url="http://lumabri.test",
                             http_client=transport(f"tools-2-{int(streaming)}.http")) as client:
        if streaming:
            with client.messages.stream(model=MODEL, max_tokens=32, messages=[{"role": "user", "content": "hi"}]) as stream:
                list(stream)
                result = stream.get_final_message()
        else:
            result = client.messages.create(model=MODEL, max_tokens=32, messages=[{"role": "user", "content": "hi"}])
        assert result.stop_reason == "tool_use"
        call = next(block for block in result.content if block.type == "tool_use")
        assert call.name == "weather" and call.input == {"city": "Roma"}

    with openai.OpenAI(api_key="local-contract-test", base_url="http://lumabri.test/v1",
                       http_client=transport(f"tools-3-{int(streaming)}.http")) as client:
        if streaming:
            with client.responses.stream(model=MODEL, input="hi", store=False) as stream:
                list(stream)
                result = stream.get_final_response()
        else:
            result = client.responses.create(model=MODEL, input="hi", store=False)
        call = next(item for item in result.output if item.type == "function_call")
        assert call.name == "weather" and json.loads(call.arguments) == {"city": "Roma"}
        assert call.call_id.startswith("call_") and result.status == "completed"

print(f"CLIENT SDK WIRE PASS: OpenAI {openai.__version__}, Anthropic {anthropic.__version__}, text and function streaming accumulators")
