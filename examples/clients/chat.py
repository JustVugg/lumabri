"""Chat with an approved Lumabri model using an official client SDK."""
import argparse
import os

parser = argparse.ArgumentParser()
parser.add_argument("protocol", choices=("openai", "anthropic"))
parser.add_argument("prompt", nargs="?", default="Hello")
parser.add_argument("--max-tokens", type=int, default=128)
args = parser.parse_args()
url = os.environ["LUMABRI_URL"].rstrip("/")
key = os.environ["LUMABRI_API_KEY"]
model = os.environ["LUMABRI_MODEL"]
messages = [{"role": "user", "content": args.prompt}]
if args.protocol == "openai":
    from openai import OpenAI
    with OpenAI(base_url=url + "/v1", api_key=key, max_retries=0) as client:
        with client.chat.completions.create(model=model, messages=messages, max_tokens=args.max_tokens, stream=True) as stream:
            for chunk in stream:
                if chunk.choices:
                    print(chunk.choices[0].delta.content or "", end="", flush=True)
else:
    from anthropic import Anthropic
    with Anthropic(base_url=url, api_key=key, max_retries=0) as client:
        with client.messages.stream(model=model, messages=messages, max_tokens=args.max_tokens) as stream:
            for text in stream.text_stream:
                print(text, end="", flush=True)
print()
