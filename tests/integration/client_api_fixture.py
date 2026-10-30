"""Prepare a disposable OLMoE fixture for client protocol tests.

The byte tokenizer needs room for function definitions and results. Weights
are copied unchanged. Give its NUL row a printable piece: sampling from this
untrained fixture can select any row, whereas the public text codec rejects
embedded NUL. This changes fixture rendering, not model arithmetic.
"""
import json
from pathlib import Path
import shutil
import sys

source, destination = map(Path, sys.argv[1:])
assert not destination.exists(), "use a fresh disposable fixture directory"
shutil.copytree(source, destination / "olmoe-tiny")
path = destination / "olmoe-tiny/config.json"
config = json.loads(path.read_text())
assert config["model_type"] == "olmoe" and config["hidden_size"] <= 128
config["max_position_embeddings"] = 2048
path.write_text(json.dumps(config, indent=2) + "\n")
path = destination / "olmoe-tiny/tokenizer.json"
tokenizer = json.loads(path.read_text())
vocab = tokenizer["model"]["vocab"]
assert config["vocab_size"] == 128 and vocab.pop("\u0100") == 0
vocab["<|fixture_nul|>"] = 0
path.write_text(json.dumps(tokenizer, ensure_ascii=False) + "\n")
