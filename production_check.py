#!/usr/bin/env python3
"""Decision gate for the three multi-host release measurements."""
import argparse
import json
from pathlib import Path


def load(path):
    with Path(path).open(encoding="utf-8") as stream:
        return json.load(stream)


def check(local, single, swarm, minimum_ratio=2.0, minimum_peers=3):
    for name, result in (("baseline-local", local),
                         ("baseline-single", single), ("swarm", swarm)):
        if result.get("tokens_match_oracle") is not True:
            raise ValueError(f"{name}: token oracle did not pass")
        if float(result.get("tok_s_single", 0)) <= 0:
            raise ValueError(f"{name}: missing positive tok_s_single")
    baseline = max(float(local["tok_s_single"]),
                   float(single["tok_s_single"]))
    speed = float(swarm["tok_s_single"])
    ratio = speed / baseline
    if int(swarm.get("peers", 0)) < minimum_peers:
        raise ValueError(f"swarm: needs at least {minimum_peers} measured peers")
    calls = swarm.get("expert_calls_per_peer") or {}
    active = {name: int(value) for name, value in calls.items()
              if int(value) > 0}
    if not active:
        raise ValueError("swarm: metric C has no active Expert peer")
    if ratio < minimum_ratio:
        raise ValueError(f"metric A ratio {ratio:.3f} is below {minimum_ratio:.3f}")
    aggregate = swarm.get("tok_s_aggregate") or {}
    missing = [str(count) for count in (1, 2, 4, 8)
               if str(count) not in aggregate]
    if missing:
        raise ValueError("metric B missing client counts: " + ",".join(missing))
    return {
        "schema": 1, "ok": True, "baseline_tok_s": baseline,
        "swarm_tok_s": speed, "speedup": ratio,
        "active_expert_peers": active,
        "tok_s_aggregate": aggregate,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--local", required=True)
    parser.add_argument("--single", required=True)
    parser.add_argument("--swarm", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--minimum-ratio", type=float, default=2.0)
    parser.add_argument("--minimum-peers", type=int, default=3)
    args = parser.parse_args()
    try:
        result = check(load(args.local), load(args.single), load(args.swarm),
                       args.minimum_ratio, args.minimum_peers)
    except (OSError, ValueError, TypeError, KeyError) as error:
        result = {"schema": 1, "ok": False, "error": str(error)}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(json.dumps(result, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
