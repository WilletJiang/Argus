# Evaluation tools

Run Python commands from the harness root. These packages share execution adapters,
trace redaction and usage accounting. Default CTest tests use fixtures and loopback
servers only; live inference is always an explicit command.

| Location | Responsibility |
| --- | --- |
| `smoke/` | Three small, fixed tasks and independent validators |
| `endurance/` | Workboard task, original checks, additional audit and parallel comparison |
| `cache/` | Controlled prefix/key/layout experiments |
| `runners/` | C executable and Codex app-server lifecycle, cutoff and wire checks |
| `trace.py`, `metrics.py` | Content-safe traces and token subset/cumulative accounting |
| `drivers/` | Optional native C evaluation executables and request decorator |

## Build and authorize

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAH_LIVE_EVAL=ON
cmake --build build --parallel
python3 -B -m host.auth login
```

The optional [host adapter](../host/README.md) reuses the subscription login in
macOS Keychain and refreshes it when needed. `exec -- COMMAND ...` passes only a
short-lived access token to a trusted evaluation process. It does not create a
Platform API key. Workspaces and tools must not receive refresh credentials.

## Small tasks

```sh
python3 -B -m host.auth exec -- python3 -B -m evals.smoke.run \
  --driver build/live_driver --output /private/fresh-smoke \
  --model gpt-6.1-sol --effort low
```

`--runner harness|codex|both` defaults to both. `--case` selects `reconcile`,
`intervals` or `update`; otherwise all three run. Each case/runner uses a fresh
workspace. The command returns nonzero when a task fails validation or execution.

| Task | Independent checks |
| --- | --- |
| reconcile | Revisions, dates, decimal rounding, joins, totals and unchanged source data |
| intervals | Boundary and malformed inputs, no mutation, generators and seeded union checks |
| update | Exact edit scope, consistent JSON/configuration, preserved historical files |

Both runners now use the same subscription Responses route and shared adapters.
Older small-task reports used the Codex CLI login route; those historical timings
are not directly interchangeable with this unified runner. System prompts, tools
and context policies still differ between implementations.

## Parallel long tasks

```sh
python3 -B -m host.auth exec -- python3 -B -m evals.endurance.run \
  --driver build/live_driver --output /private/fresh-endurance \
  --model gpt-6.1-sol --effort xhigh --duration 600 --repetitions 3
```

The task implements the complete Workboard API, durable storage, query engine,
CLI, regression tests and documentation. The prompt does not disclose the external
observation ceiling. No follow-up forces a stopped model to continue. A wire probe
checks Codex's requested model/effort before inference; two live file-read preflights
must pass before a barrier releases all six task runs. Probe usage stays separate.

The task, specification and grader hashes are recorded before execution. A copy
of the grader runs outside the model workspaces. Each run is graded even after a
failure or cutoff. Preserve failed attempts and label reruns explicitly. Natural
completion, correctness, missing deliverables and externally stopped work must be
reported separately. The additional audit runs with:

```sh
python3 -B -m evals.endurance.quality /absolute/path/to/workspace
```

Do not modify submitted implementations before grading. Generated tests should also
be rerun independently. The additional audit includes storage-layout probes; review
a fixture failure before assigning it to the implementation. The unspecified
Conflict/NotFound precedence case remains an unscored observation.

## Cache controls

```sh
python3 -B -m host.auth exec -- python3 -B -m evals.cache.run \
  --driver build/cache_driver --output /private/fresh-cache --rounds 2 --parallel
```

Fixed reference data supports cold, identical and growing requests. Equal-length
fresh group IDs isolate trials. Variants compare JSON keys and instruction layout;
the optional phase uses three concurrent clients. These are protocol probes, not
a task-quality benchmark. Keep a baseline binary to isolate a transport change.

The study records accepted parameters, actual model/effort, key echoes, canonical
input fingerprints, usage and optional diagnostics. A returned `unavailable`
diagnostic is not a classified hit or miss. Missing usage remains unknown, and
failures are not automatically replaced.

The native agent driver also accepts these evaluation-only environment variables:

- `AH_EVAL_CACHE_KEY=session|omit`: the durable UUID by default, mirrored into
  HTTP `session-id`; omission removes both.
- `AH_EVAL_CACHE_DIAGNOSTICS=0|1`: request previous-response comparisons (default 0).
- `AH_EVAL_TRACE_REQUESTS=0|1`: emit prepared requests to the evaluator (default 0).

`evals.trace.Trace` fingerprints prepared requests before persistence. Do not save
raw driver stdout when request tracing is enabled. Ordinary model/tool events still
contain task data. Token summaries include cached tokens within input and reasoning
within output; never add these subsets twice. Interrupted calls may omit usage.

## Boundaries and records

C tools use bounded native file access and an evaluation-only Codex sandbox wrapper
for process execution. Codex uses an isolated app-server home with external skills,
plugins, memory and child agents disabled. The same endpoint does not make prompts,
tool APIs or context windows identical. Parallel repetitions may share provider
caches and load. Small task counts cannot establish broad superiority.

Keep generated code, databases and raw traces outside the repository. Remove them
after retaining the necessary compact measurements; retain the Keychain login.
Historical reports and ledgers live in [docs/evaluations](../docs/evaluations/README.md).
The layout changed after those experiments; their recorded source hashes refer to
the original commits. Prompt/specification contents remain unchanged.
