#!/usr/bin/env python3
"""Decision gate for the three multi-host release measurements."""
import argparse
import json
import math
from pathlib import Path


def load(path):
    with Path(path).open(encoding="utf-8") as stream:
        return json.load(stream)


def finite_rate(value):
    if type(value) not in (int, float):
        return None
    try:
        rate = float(value)
    except OverflowError:
        return None
    return rate if math.isfinite(rate) and rate > 0 else None


def check(local, single, swarm, minimum_ratio=2.0, minimum_peers=3):
    if finite_rate(minimum_ratio) is None or type(minimum_peers) is not int or minimum_peers < 1:
        raise ValueError("invalid release thresholds")
    rates = {}
    identity = None
    for name, result in (("baseline-local", local),
                         ("baseline-single", single), ("swarm", swarm)):
        if result.get("mode") != name:
            raise ValueError(f"{name}: report mode does not match")
        model, family = result.get("model"), result.get("family")
        if not isinstance(model, str) or not model or not isinstance(family, str) or not family:
            raise ValueError(f"{name}: missing model or family")
        if identity is None:
            identity = (model, family)
        elif (model, family) != identity:
            raise ValueError(f"{name}: model or family differs from baseline-local")
        if result.get("tokens_match_oracle") is not True:
            raise ValueError(f"{name}: token oracle did not pass")
        rate = finite_rate(result.get("tok_s_single"))
        if rate is None:
            raise ValueError(f"{name}: missing finite positive tok_s_single")
        rates[name] = rate
    baseline = max(rates["baseline-local"], rates["baseline-single"])
    speed = rates["swarm"]
    ratio = speed / baseline
    if not math.isfinite(ratio):
        raise ValueError("metric A ratio is not finite")
    peers = swarm.get("peers")
    if type(peers) is not int or peers < minimum_peers:
        raise ValueError(f"swarm: needs at least {minimum_peers} measured peers")
    calls = swarm.get("expert_calls_per_peer")
    if not isinstance(calls, dict):
        raise ValueError("swarm: metric C requires peer counters")
    for name, value in calls.items():
        if not isinstance(name, str) or not name or type(value) is not int or value < 0:
            raise ValueError("swarm: metric C requires named non-negative integer counters")
    if len(calls) != peers:
        raise ValueError("swarm: metric C peer count differs from counters")
    active = {name: value for name, value in calls.items() if value > 0}
    if not active:
        raise ValueError("swarm: metric C has no active Expert peer")
    if ratio < minimum_ratio:
        raise ValueError(f"metric A ratio {ratio:.3f} is below {minimum_ratio:.3f}")
    aggregate = swarm.get("tok_s_aggregate") or {}
    missing = [str(count) for count in (1, 2, 4, 8)
               if str(count) not in aggregate]
    if missing:
        raise ValueError("metric B missing client counts: " + ",".join(missing))
    for count in (1, 2, 4, 8):
        if finite_rate(aggregate[str(count)]) is None:
            raise ValueError(f"metric B has invalid rate for {count} clients")
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
