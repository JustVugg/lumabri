#!/usr/bin/env python3
"""Reproducible multi-host Lumabri benchmark.

The runner deliberately treats deployment as data.  A spec supplies commands
for already-installed Lumabri binaries; commands may run locally or through
ssh.  Each inference command must finish with one JSON object containing:

  {"token_ids":[1,2], "decode_tokens":2, "decode_seconds":0.42,
   "bytes":{"cas":0,"expert":12,"segment":34}}

This keeps the performance gate independent from terminal wording and makes a
failed/mismatching generation an invalid sample, never a fast sample.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


FAMILIES = {"glm", "inkling", "kimi_k3", "qwen36", "olmoe", "deepseek_v4"}


def remote_command(host: str, command: list[str]) -> list[str]:
    if host in ("", "local", "localhost"):
        return command
    return ["ssh", "-o", "BatchMode=yes", host, "--", shlex.join(command)]


def render(value: str, fields: dict[str, Any]) -> str:
    return value.format_map({key: str(val) for key, val in fields.items()})


def run_json(host: str, command: list[str], fields: dict[str, Any], timeout: float,
             environment: dict[str, Any] | None = None) -> dict:
    rendered = [render(part, fields) for part in command]
    values = {str(key): render(str(value), fields)
              for key, value in (environment or {}).items()}
    for key in values:
        if (not key or not key.isascii() or
                not (key[0].isalpha() or key[0] == "_") or
                any(not (char.isalnum() or char == "_") for char in key)):
            raise ValueError(f"invalid environment variable name: {key!r}")
    local = host in ("", "local", "localhost")
    if not local and values:
        rendered = ["env", *(f"{key}={value}" for key, value in values.items()),
                    *rendered]
    argv = remote_command(host, rendered)
    process_environment = None
    if local and values:
        process_environment = os.environ.copy()
        process_environment.update(values)
    proc = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=timeout, check=False,
                          env=process_environment)
    if proc.returncode:
        raise RuntimeError(f"command failed ({proc.returncode}): {shlex.join(argv)}\n{proc.stderr[-2000:]}")
    for line in reversed(proc.stdout.splitlines()):
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            return obj
    raise RuntimeError(f"command produced no JSON object: {shlex.join(argv)}")


def validate_sample(sample: dict, oracle: list[int]) -> dict:
    ids = sample.get("token_ids")
    count = sample.get("decode_tokens")
    seconds = sample.get("decode_seconds")
    if not isinstance(ids, list) or any(not isinstance(v, int) for v in ids):
        raise ValueError("sample has no integer token_ids array")
    if ids != oracle:
        raise ValueError(f"tokens differ from oracle: {ids} != {oracle}")
    if not isinstance(count, int) or count <= 0 or count != len(ids):
        raise ValueError("decode_tokens must equal len(token_ids) and be positive")
    if not isinstance(seconds, (int, float)) or not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("decode_seconds must be finite and positive")
    byte_counts = sample.get("bytes", {})
    for key in ("cas", "expert", "segment"):
        value = byte_counts.get(key, 0)
        if not isinstance(value, int) or value < 0:
            raise ValueError(f"bytes.{key} must be a non-negative integer")
    return {"tok_s": count / seconds, "seconds": seconds,
            "bytes": {key: int(byte_counts.get(key, 0))
                      for key in ("cas", "expert", "segment")}}


def peer_counters(spec: dict, timeout: float) -> dict:
    probe = spec.get("probe")
    if not probe:
        return {}
    obj = run_json(probe.get("host", "local"), probe["command"], {}, timeout)
    return {peer["name"]: int(peer.get("expert", {}).get("calls", 0))
            for peer in obj.get("peers", [])}


def one_sample(spec: dict, fields: dict[str, Any], oracle: list[int], timeout: float) -> dict:
    chatter = spec["chatter"]
    raw = run_json(chatter.get("host", "local"), chatter["command"], fields,
                   timeout, chatter.get("env"))
    return validate_sample(raw, oracle)


def client_batch(spec: dict, clients: int, fields: dict[str, Any],
                 oracle: list[int], timeout: float) -> tuple[list[dict], float]:
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
        jobs = [pool.submit(one_sample, spec, fields | {"client": i}, oracle, timeout)
                for i in range(clients)]
        samples = [job.result() for job in jobs]
    elapsed = time.monotonic() - started
    total_tokens = sum(len(oracle) for _ in samples)
    return samples, total_tokens / elapsed


def median(values: list[float]) -> float:
    if not values:
        raise ValueError("cannot take median of empty list")
    return statistics.median(values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mode", choices=("baseline-local", "baseline-single", "swarm"),
                        default="swarm")
    parser.add_argument("--clients", default="1,2,4,8")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=900.0)
    args = parser.parse_args()

    spec = json.loads(args.spec.read_text(encoding="utf-8"))
    family = spec.get("family")
    if family not in FAMILIES:
        raise SystemExit(f"family must be one of {sorted(FAMILIES)}")
    oracle = spec.get("oracle_token_ids")
    if not isinstance(oracle, list) or not oracle:
        raise SystemExit("spec.oracle_token_ids must be a non-empty array")
    clients = [int(v) for v in args.clients.split(",")]
    if any(v <= 0 for v in clients) or args.runs < 1 or args.warmup < 0:
        raise SystemExit("clients/runs/warmup must be positive")

    fields = {"mode": args.mode, "prompt": spec.get("prompt", ""),
              "max_tokens": len(oracle), "model": spec["model"]}
    for _ in range(args.warmup):
        one_sample(spec, fields | {"client": 0}, oracle, args.timeout)

    before = peer_counters(spec, args.timeout)
    single, aggregate, all_bytes = [], {}, {"cas": 0, "expert": 0, "segment": 0}
    for count in clients:
        rates = []
        for _ in range(args.runs):
            samples, rate = client_batch(spec, count, fields, oracle, args.timeout)
            rates.append(rate)
            for sample in samples:
                if count == 1:
                    single.append(sample["tok_s"])
                for key, value in sample["bytes"].items():
                    all_bytes[key] += value
        aggregate[str(count)] = median(rates)
    after = peer_counters(spec, args.timeout)
    calls = {name: after.get(name, 0) - before.get(name, 0)
             for name in sorted(set(before) | set(after))}

    result = {
        "schema": 1, "model": spec["model"], "family": family,
        "mode": args.mode, "runs": args.runs, "warmup": args.warmup,
        "peers": len(calls), "tok_s_single": median(single),
        "tok_s_aggregate": aggregate, "expert_calls_per_peer": calls,
        "bytes": all_bytes, "tokens_match_oracle": True,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temp = args.output.with_suffix(args.output.suffix + ".tmp")
    temp.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temp, args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"swarm_bench: {exc}", file=sys.stderr)
        raise SystemExit(1)
