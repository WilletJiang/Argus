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

Generated outputs from the 2026-11-03 run were removed after validation. Keep future
run outputs outside the source tree and clean them up when the comparison is complete.


## Long-task paired trials

`endurance_run.py` runs three fresh copies of one multi-component Workboard release
task per runner. Both runners request `gpt-6.1-terra` with `xhigh` through the same
plan-authorized public Responses endpoint. Codex uses app-server with an isolated
home and no external plugins, memory or child agents; its native prompt and tools
still differ from the C harness. The user prompt and specification contain no
evaluation duration. No follow-up prompt forces a stopped agent to continue.

```sh
python3 -B evals/endurance_run.py --driver build/live_driver   --output /private/fresh-evaluation --model gpt-6.1-terra --effort xhigh   --duration 600 --repetitions 3
```

The host supplies `AH_ACCESS_TOKEN` in memory. It is not a Platform API key. Two
separate access probes must complete before a barrier releases all six task runs.
Each probe reads a fixture file through a tool before responding, so a dropped
streamed tool call cannot pass as a successful text-only access check.
Their usage is recorded separately. The external controller interrupts active runs
at the observation boundary, then allows a short shutdown grace period. Execution
can end sooner on completion, failure or a request for unavailable human input.
Normal completion is not classified as a persistence failure merely because it
finishes before the observation boundary.

The C adapter raises its non-time limits for this evaluation (1024 model calls,
4096 tools, 8 MiB context, 4 MiB response, 1 MiB arguments). File-tool capture and
16 KiB model-result views remain unchanged. Codex retains its own context machinery.
These are operational controls, not identical tokenizer budgets. Both process
tool environments exclude credentials; workspaces contain only synthetic files.

The frozen 33 behavioral checks cover persistence, transactions, idempotency,
leases, dependency order, analytics and CLI behavior. They do not establish full
production correctness. Additional documentation/tests and unchanged input hashes
are reported separately. The grader is never sent to the agent. Prompt/spec/grader
hashes are recorded before execution. The live runner snapshots its grader outside the agent workspaces before execution;
never edit criteria after seeing model outputs. The reported comparison also used
a separate frozen copy of the runner itself.

Report each trial rather than selecting the best. Input tokens include cached
input; output tokens include reasoning output, so those subsets must not be added
again. The C harness sums response usage; app-server exposes cumulative thread
usage, for which the latest total is used. Interrupted requests can omit usage:
`usage_complete=false` marks observed lower bounds, never an assumed zero cost.
Tool-active minute buckets measure observed tool starts, not proof of productive
reasoning during every intervening second. Natural completion and graded results
must be considered together.

`test_endurance.py` uses fake local executables to verify barriers, cutoff behavior
and usage accounting without external inference. Keep run workspaces/traces out of
the repository and remove them after recording the aggregate measurements.

After-run quality review uses `endurance_quality.py WORKSPACE` and is reported
separately from the frozen 33-check score. The 2026-11-03 measurements and all
confirmed token usage, including the interrupted pilot, are recorded in
[the evaluation report](../docs/endurance-evaluation.md).

## Cache diagnostics

The macOS [host adapter](../host/README.md) keeps the subscription login in Keychain
across evaluations. Test cleanup removes generated workspaces, not the saved login.
Build `cache_driver` with `AH_LIVE_EVAL=ON`, then run:

```sh
python3 -B host/auth.py login --then python3 -B evals/cache_study.py \
  --driver build/cache_driver --output /private/fresh-cache-study --rounds 2 --parallel
```

The controlled study uses the native C HTTP/SSE transport with synthetic fixed
reference records. It compares a stable key versus omission, top-level instructions
versus a developer input message, and cold/identical/growing requests. Equal-length
fresh group identifiers separate trials; every case uses the same records and
increments. The optional parallel phase runs three clients per key variant.
These are cache protocol probes, not a task-quality or endurance benchmark.

A capability probe checks the actual served model and reasoning effort. It tests
`prompt_cache_options.comparison_response_id` and changes instructions/tools as
positive controls. A rejected diagnostics parameter is recorded; remaining cases
then omit it. Accepted parameters without returned diagnostics do not prove server
support. No retry silently replaces a failed trial. Missing usage stays unknown.

Each request gets content-free SHA-256 fingerprints of instructions, tools,
settings and individual input items. Reports include prefix preservation, cached
and uncached input, optional cache writes/diagnostics, returned model/tier and
latency. Hash equality checks client serialization, not server tokenization.
Input includes its cached subset; output includes its reasoning subset.

The real agent `live_driver` supports the same investigation via:

- `AH_EVAL_CACHE_KEY=session|omit` (default: the durable session UUID, mirrored
  by the transport into HTTP `session-id`; omission removes both).
- `AH_EVAL_CACHE_DIAGNOSTICS=0|1` (default: 0).
- `AH_EVAL_TRACE_REQUESTS=0|1` (default: 0).

Request tracing exposes raw request bodies to the driver's stdout consumer.
`endurance_runner.Trace` hashes them before writing `cache-metrics.jsonl` or the
ordinary trace; do not pipe raw events directly to a persistent file. Ordinary
model/tool events still contain task data. These flags are evaluation-only and do
not change the public C configuration or credential handling.

The completed [cache routing investigation](../docs/cache-evaluation.md) includes
JSON-only and session-header controls, native file-tool loops and three concurrent
clients. Keep the old binary when comparing a transport change; run the same
`run_case` workload through each binary and retain failed cases. The response key
echo check detects a server-rewritten key without storing the key itself.
