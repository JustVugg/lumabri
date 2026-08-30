#!/usr/bin/env python3
"""Long-running token-oracle soak using the same chatter contract as the bench."""
import argparse
import json
import statistics
import time
from pathlib import Path

import swarm_bench


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if args.seconds < 1 or args.interval < 0:
        parser.error("seconds must be positive and interval non-negative")
    spec = json.loads(Path(args.spec).read_text(encoding="utf-8"))
    oracle = [int(value) for value in spec["oracle_token_ids"]]
    command = spec["chatter"]["command"]
    host = spec["chatter"].get("host", "local")
    environment = spec["chatter"].get("env", {})
    fields = {"model": spec["model"], "prompt": spec.get("prompt", ""),
              "max_tokens": len(oracle), "mode": "swarm", "client": 0}
    started = time.time()
    deadline = started + args.seconds
    rates, failures = [], []
    runs = 0
    while time.time() < deadline:
        runs += 1
        try:
            sample = swarm_bench.run_json(host, command, fields, args.timeout,
                                          environment)
            valid = swarm_bench.validate_sample(sample, oracle)
            rates.append(valid["tok_s"])
        except Exception as error:  # keep soaking to expose repeated failures
            failures.append({"run": runs, "error": str(error),
                             "at_seconds": time.time() - started})
        if args.interval:
            time.sleep(min(args.interval, max(0.0, deadline - time.time())))
        if runs % 10 == 0:
            print(f"soak: {runs} runs, {len(failures)} failures", flush=True)
    result = {
        "schema": 1, "ok": not failures and bool(rates),
        "model": spec["model"], "duration_seconds": time.time() - started,
        "runs": runs, "successful_runs": len(rates), "failures": failures,
        "tokens_match_oracle": not failures and bool(rates),
        "tok_s_median": statistics.median(rates) if rates else 0.0,
        "tok_s_min": min(rates) if rates else 0.0,
        "tok_s_max": max(rates) if rates else 0.0,
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(output)
    print(json.dumps(result, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
