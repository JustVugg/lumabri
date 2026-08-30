# Multi-host performance gate

`swarm_bench.py` records the three release metrics without scraping the TUI:

- A: median decode tokens/s for one chat;
- B: aggregate decode tokens/s for 1, 2, 4 and 8 concurrent chats;
- C: the change in executed expert calls for every named peer.

The run is invalid unless every sample returns exactly the configured oracle
token IDs. Use temperature zero, one fixed prompt and build-compatible peers.
Run at least one warm-up and five measured repetitions. Compare `swarm` with
both `baseline-local` (stock local Colibri) and `baseline-single` (one resident
Segment on one machine); do not compare against an intentionally starved
four-process origin.

The chatter command in the spec is an adapter: `segment_chat --json` already
implements it, and a local-engine wrapper can implement the same small
contract for `baseline-local`. Its final stdout
line must be a JSON object with `token_ids`, `decode_tokens`,
`decode_seconds`, and optional CAS/Expert/Segment byte counters. The example
spec shows the contract. Commands run locally when `host` is `local`, and via
non-interactive SSH otherwise. An optional `chatter.env` object is applied to
the local process or through `env` after SSH; its values accept the same
placeholders as the command. Lumabri does not install keys or open hosts.

Example:

```sh
./swarm_bench.sh run.json results/deepseek-swarm.json --mode swarm
./swarm_bench.sh run.json results/deepseek-local.json --mode baseline-local
```

Use 0, 1, 2, then 3 compute donors on the same prompt. A release candidate
meets the speed objective only when the three-donor median A is at least 2x
the best single-machine baseline and no additional READY peer makes it slower.
