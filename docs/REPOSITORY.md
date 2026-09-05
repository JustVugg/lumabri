# Repository layout and retention audit

This is a developer map, not a statement that every runtime or platform is
ready for household use. The public README is unchanged by this cleanup.

## Layout

| Location | Responsibility |
| --- | --- |
| `src/ui/` | C catalogue renderer, typed UI state and household launcher |
| `tests/c/` | C unit tests and integration-test clients |
| `tests/ui_text_test.py` | English application-copy and rendered-help regression guard |
| Root `*_test.sh`, `*_test.py` | Integration runners; still launched from the root |
| `lumabri.c` | CLI, chat editor/streaming and orchestration; still a large translation unit |
| `lumabri_home*.h` | Household consent transaction and runtime orchestration |
| `segment_*.c`, `lumabri_segment*`, `segment_colibri.h` | Segment workers, Edge chat, discovery and Colibri integration |
| `tracker.c`, `maintainer.c`, `lumashim.c` | Discovery/control, checkpoint distribution and block mirror |
| `lumabri_planner.h`, `lumabri_cluster.h`, `lumabri_calibration.h` | Model sizing, placement and calibration records |
| `lumabri_machine.*`, scheduler and governor headers | Hardware inventory, resource budgets and admission |
| `expert_engines/`, `engine_patches/`, Expert bridge files | Optional Expert/Hybrid and upstream compatibility |
| `deploy/`, root deployment/release documents | Existing deployment and release tooling |
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
proof that every function is reachable. No production source was deleted in
this pass. The 30 relocated C test sources are preserved, not removed.

## Further cleanup boundaries

Next candidates are integration scripts and historical engineering documents,
then splitting chat lifecycle/editor code out of `lumabri.c`. Each requires
updating path assumptions, install rules and tests together. Do not remove
the compatibility names or engine hooks before retiring their callers.

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
