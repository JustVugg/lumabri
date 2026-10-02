#!/usr/bin/env python3
"""Adapt upstream's production-tokenizer cases to our generated Tiny Qwen.

v1.12.1's real Edge oracle tests six production added-token IDs around 248k.
Our math fixture has 320 rows and an identity-byte tokenizer. Give it six
added tokens at reserved fixture rows and change only those expected IDs in
a generated oracle copy. Keep every encode/decode and numerical assertion.
Never edit the upstream checkout or accept a production tokenizer here.
"""
import argparse
import json
from pathlib import Path

CASES = (
    ("<tool_call>", 248058), ("</tool_call>", 248059),
    ("<tool_response>", 248066), ("</tool_response>", 248067),
    ("<think>", 248068), ("</think>", 248069),
)


def fixture_tokenizer(path):
    data = json.loads(path.read_text())
    vocab = data["model"]["vocab"]
    if data.get("added_tokens") or data["model"].get("merges") or len(vocab) != 320:
        raise ValueError("expected the untouched generated 320-token Edge fixture")
    for index, (piece, _) in enumerate(CASES, 256):
        if vocab.get(f"<|edge_fixture_{index}|>") != index or piece in vocab:
            raise ValueError("not a generated Tiny tokenizer; refusing to overwrite")
        del vocab[f"<|edge_fixture_{index}|>"]
        data["added_tokens"].append({"id": index, "content": piece,
            "single_word": False, "lstrip": False, "rstrip": False,
            # Qwen's tool/thinking markers are added but not special: they
            # must remain visible to the protocol parser when decoding.
            "normalized": False, "special": False})
    return json.dumps(data, ensure_ascii=False) + "\n"


def fixture_oracle(source):
    for index, (piece, production_id) in enumerate(CASES, 256):
        anchor = '{"%s", %d}' % (piece, production_id)
        if source.count(anchor) != 1:
            raise ValueError("Qwen added-token oracle changed: " + anchor)
        source = source.replace(anchor, '{"%s", %d}' % (piece, index), 1)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tokenizer", type=Path, action="append", required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        raise ValueError("oracle output must not replace upstream source")
    # Validate every fixture before changing any output.
    oracle = fixture_oracle(args.source.read_text())
    fixtures = [(path, fixture_tokenizer(path)) for path in args.tokenizer]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(oracle)
    for path, data in fixtures:
        path.write_text(data)
    print("Qwen Edge oracle: all six added-token checks retained at Tiny IDs 256–261")


if __name__ == "__main__":
    main()
