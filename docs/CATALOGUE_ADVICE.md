# Catalogue advice

The catalogue compares only verified, resident plans that fit the computers
you selected and have a usable checkpoint source. Nothing is selected or
downloaded automatically. Participating donors still approve the allocation.

- **Lowest RAM reservation**: the smallest sum of the planned process budgets,
  including the Edge host. This is reserved capacity, not predicted RSS.
- **Largest resident checkpoint**: the largest checkpoint by bytes among the
  admitted plans. More bytes do not imply better answers.
- **Fastest last run**: the highest observed decode rate among at least two
  plans with current, matching measurement records. Different prompts, model
  tokenizers and machine load make this an observation, not a controlled
  quality/speed benchmark or a promise about your next conversation.

The compact row displays one applicable label, prioritizing observed speed,
then lowest RAM, then checkpoint size. With fewer than two eligible choices
there is no comparative advice. Unknown speeds are never invented. Selecting
different computers clears advice immediately while the planner refreshes.

The TUI and JSON use the same computed snapshot; model families are not
hard-coded in the advisor. No host, donor or model is selected on your behalf.

Run `make test_catalogue_advice && ./test_catalogue_advice` for the independent
selection, invalid-input, unknown-speed and stable tie-break tests.
