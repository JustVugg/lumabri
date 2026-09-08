#!/usr/bin/env python3
"""Normalize upstream independent V4 references; never generate tokens in C."""
import argparse
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("model", type=Path)
    args = p.parse_args()
    ref = json.loads((args.model / "ref.json").read_text())
    assert ref["source"] == "transformers"
    for name, case in ref["cases"].items():
        assert name in ("short", "compressed", "long")
        prompt, full = case["prompt_ids"], case["greedy_full_ids"]
        assert full[:len(prompt)] == prompt and len(full) > len(prompt)
        (args.model / f"edge_ref_{name}.json").write_text(json.dumps(
            dict(prompt_ids=prompt, full_ids=full), indent=2) + "\n")


if __name__ == "__main__":
    main()
