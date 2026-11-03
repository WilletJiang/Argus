# Small live comparison suite

These three fixed, synthetic tasks measure completed workspace outputs. They
contain no user data and require no external network. `cases.py` makes no model
calls and handles no credentials. Its standard-library API is:

```python
from pathlib import Path
from cases import CASES

case = CASES[0]
workspace = Path("/absolute/path/to/an/empty/workspace")
prompt = case.prepare(workspace)
# Run one agent with this prompt and workspace, then wait for it to stop.
result = case.validate(workspace)  # {"pass": bool, "details": list[str]}
```

| Case | Objective | Checks |
| --- | --- | --- |
| `reconcile` | Reconcile invoices, payments, customers, and FX into JSON | Revision precedence, date/state filtering, decimal rounding, joins, totals, preserved sources; an instruction-shaped CSV note remains data |
| `intervals` | Repair an existing Python function from its contract | Empty, nested, touching, negative and duplicate intervals; malformed inputs, bool endpoints, generators, no input mutation; 50 fixed-seed interval-union checks |
| `update` | Apply a constrained migration across files | Exact edit scope, consistent values, JSON manifest, historical text and sentinel preservation, no extra files |

Prepare a fresh empty workspace separately for each runner and each repetition.
Use the same returned prompt and initial bytes, model slug, reasoning setting,
wall-time/output/tool budgets where supported, and no inherited conversation.
Record unavoidable differences in system prompts, tool APIs, approval handling,
context limits, and effective budgets. Do not count a self-reported completion as
a pass; run the validator after the runner exits. Report each case, pass count,
elapsed time, token use when available, and failures. Do not tune prompts or
validators after inspecting model results. Repeated attempts should be reported
separately rather than selecting the best answer.

Keep this module, expected outputs, and evaluation logs outside the model's
workspace; pass the prompt only, not validator paths or source. This separation
is not an OS sandbox. An unrestricted process tool can potentially inspect host
files or use the network despite the task instruction. Meaningful adversarial
isolation requires a sandbox supplied by the runner. The Python validator runs
submitted code in a child process with a 10-second timeout, isolated Python
imports, and a minimal environment; it does not provide an OS security boundary.

This is a small smoke evaluation of bounded, self-contained work. It cannot
establish broad agent quality, long-horizon reliability, security against a
hostile submission, or superiority across models and real projects. A perfect
score may indicate a ceiling on these tasks. A single failure can reflect the
runner or tool interface as well as the model; retain the trace to distinguish
those causes. Hidden checks test only requirements stated in the supplied prompt.

## Running the comparison

Build the live adapter explicitly; it is never included in default tests:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAH_LIVE_EVAL=ON
cmake --build build --target live_driver
python3 evals/run_codex.py --output /private/evaluation/codex
# The host supplies an authorized subscription bearer in AH_ACCESS_TOKEN:
python3 evals/run_harness.py --driver build/live_driver --output /private/evaluation/harness
```

Each output directory must be fresh. The harness adapter injects `reasoning.effort=low`
and registers an evaluation-only exec wrapper around `codex sandbox -P :workspace`.
Codex is used only for OS isolation in that wrapper: inference, context, state and
scheduling remain in the C harness. Existing file tools run with their root-bound
policy. The comparison is not tool-for-tool identical: Codex has its own prompt,
context and tool implementation, while the harness has a small native tool set.
The harness adapter uses internal executor helpers and is not an example of the
stable embedding API.

Codex baseline uses its existing ChatGPT login and CLI route; the harness uses an
independent plan-authorized OAuth credential with the public Responses endpoint.
These distinct service routes and cache behavior can affect latency and usage.
Neither runner creates an API key or copies credential files. Trace files contain
synthetic task data but should still be reviewed before publication.

A timeout and output correctness are separate observations. The grader runs even
when a runner times out. Retain failed attempts and label reruns explicitly.

See [the 2026-11-03 live report](results/2026-11-03.md).
