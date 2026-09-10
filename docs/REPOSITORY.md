# Repository layout and retention audit

This is a developer map, not a statement that every runtime or platform is
ready for household use. The public README documents the tested TUI path.

## Layout

| Location | Responsibility |
| --- | --- |
| `src/ui/` | C workspace canvas, catalogue, household launcher and chat editor |
| `tests/c/` | C unit tests and integration-test clients |
| `tests/ui_text_test.py` | English application-copy and rendered-help regression guard |
| `tests/integration/` | Shell integration runners and household/chat/inventory PTY tests |
| `lumabri.c` | CLI, streaming and orchestration; still a large translation unit |
| `lumabri_home*.h` | Household consent transaction and runtime orchestration |
| `segment_*.c`, `lumabri_segment*`, `segment_colibri.h` | Segment workers, Edge chat, discovery and Colibri integration |
| `tracker.c`, `maintainer.c`, `lumashim.c` | Discovery/control, checkpoint distribution and block mirror |
| `lumabri_planner.h`, `lumabri_cluster.h`, `lumabri_calibration.h` | Model sizing, placement and calibration records |
| `src/planner/` | Model-independent catalogue advice; no UI rendering or weight loading |
| `lumabri_machine.*`, scheduler and governor headers | Hardware inventory, resource budgets and admission |
| `expert_engines/`, `engine_patches/`, Expert bridge files | Optional Expert/Hybrid and upstream compatibility |
| `deploy/`, root deployment/release documents | Existing deployment tooling and the minimal household launcher |
| `tools/package_household.py` | Allowlisted native candidate assembly; never copies models or credentials |
| `tools/prepare_engine_source.py` | Bounded source-only Colibri build copy; excludes checkpoints and generated outputs |
| `build/`, local executables, `__pycache__/`, tiny fixtures | Generated/ignored artifacts, not production source files |

The root Makefile remains the supported build entry point. Test executable
names stay unchanged so integration scripts and CI keep working. Shared
headers still live at the root; Make supplies that include directory through
`CPPFLAGS`, including when custom warning flags are provided.

## What this pass retained, and why

- `lumibri_client.h` is a compatibility include used by the engine patches,
  not an unused duplicate of `lumabri_client.h`.
- `lumi_v4_bridge.c` and `lumi_v4_ext.h` are used by the multi-unit DeepSeek
  build and the Segment Hybrid archive. They cannot simply be removed.
- `expert_engines/` and `engine_patches/` remain dependencies of existing
  targets. Household Segment disables remote Expert routing; this does not
  make the Expert source unreachable from every supported build.
- Relay, identity and verification tests protect existing protocols. Public
  networking is outside the household milestone, not proven dead code.
- `segment_budget_probe.c` and `swarm_rows_bench.c` are diagnostic tools with
  explicit Make targets, not application entry points to merge into the CLI.
- Generated binaries are ignored build products. They were not deleted just
  to make a directory listing shorter; that would prevent an immediate test
  without reducing the tracked repository.

This audit follows build and include references. It is **not** a whole-program
proof that every function is reachable. C and integration tests are preserved,
not removed. Shell runners change to the repository root before using binaries.

## Retired code

- The old interactive swarm-address panel and automatic chat-plus-donation
  picker, together with helpers that only those panels called.
- The boxed chat splash. The existing wordmark remains in the workspace.
- The isolated sample-data TUI preview, its screenshot tool and its tests now
  that the approved design runs on real household state. The production-frame
  capture tool and screenshots remain.

Bare interactive `lumabri chat` opens the same household workspace as
`lumabri`. Explicit CLI arguments and script usage stay available; donation
requires an explicit role or household approval. Old saved CLI settings are
still read, not erased or conflated with household membership keys.

The removed sources remain recoverable in Git history. No user configuration,
model, key, cache or untracked workspace file is deleted.

## Further cleanup boundaries

The chat editor is an internal include of `lumabri.c`: this reduces file size
without pretending the editor has an independent lifecycle. Further separation
of streaming/session ownership needs its own tests and API boundary. Historical
engineering documents and active engine hooks are retained; do not remove
compatibility names before retiring their callers.

Keep checkout models, user keys and `~/.lumabri` outside source-cleanup work.
Never treat a local file as disposable just because Git does not track it.

## Checking this layout

From the repository root, with a compatible Colibri checkout:

```sh
make lumabri test-chat-ui ENGINE=/path/to/colibri/c
make test ENGINE=/path/to/colibri/c
```

The first target checks the UI without loading a model. The full suite also
needs local TCP sockets and engine build prerequisites. Neither command is
a native Windows/macOS or a physical-LAN certification.
