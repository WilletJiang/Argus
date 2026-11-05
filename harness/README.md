# ah — a standalone C agent harness

A C17 library and headless executable for durable tool-using agents. It runs
without Argus, Swift, AppKit, a browser UI, or an agent framework. Argus is a
prospective client, not a dependency. The project is independently buildable and
installable so it can move to its own repository without an architectural rewrite.

The supported model protocol is OpenAI Responses over HTTP/SSE, including the
ChatGPT-plan request shape. Authentication is supplied by the embedding host;
this library does not read Codex credentials or implement an OAuth browser flow.
The optional [macOS host adapter](host/README.md) provides persistent ChatGPT login
with Keychain storage and serialized refresh, without adding dependencies to C clients.

## Build and verify

Requires CMake 3.20+, a C17 compiler, libcurl 7.68+ built with asynchronous DNS,
SQLite 3, and pthreads. Python 3 is used by integration tests and the optional host adapter. JSON parsing
uses vendored yyjson 0.12.0. Linux process tools require glibc 2.34+; macOS uses
native spawn actions. Unsupported process isolation primitives fail closed.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/ah_embed
```

Sanitizers: configure a separate build with `-DAH_SANITIZE=ON`. The tests use mock
providers, temporary workspaces, and loopback HTTP only: no credentials, paid API,
or external messages. `scripts/check-standalone.sh` copies this project into a
fresh directory, builds/tests/installs it, and builds an independent C client
against the installed `ah::ah` package.

## Project layout

- `include/ah/`, `src/`: public C API and runtime, grouped by responsibility.
- `host/`: optional subscription login; use `python3 -B -m host.auth`.
- `tests/`: offline contracts; `benchmarks/`: local overhead measurements.
- `evals/`: smoke/endurance/cache workloads sharing execution and telemetry.
- `docs/`: current contracts and architecture; `docs/evaluations/`: historical records.

See the [code map](docs/architecture.md), [evaluation commands](evals/README.md),
and [performance investigation](docs/performance.md).

## What is implemented

- Persistent sessions and an ordered semantic event journal; idempotent user commands.
- A complete model/tool loop with successful-response barrier, native response items,
  bounded context/output, deadlines, and model/tool call budgets.
- Durable, operation-bound approval, `ask` and `wait` requests; explicit answers.
- Cancellation and bounded in-run steering, with distinct queued/durable acknowledgments.
- Crash recovery that preserves uncertain effects; explicit reconciliation rather than
  blind re-execution of writes.
- Completed-session forks and explicit context checkpoints, including settled stopped turns.
- Stable per-session prompt cache keys, retained across reopen, resume and checkpoints.
- Streaming HTTPS with connection reuse, bounded parser buffers, strict UTF-8/JSON,
  conservative retries, and interruption during stalled network I/O or backoff.
- Completed streaming output-item collection, including providers whose terminal
  response omits output; tool dispatch still waits for successful response completion.
- Paged UTF-8 reads, directory listing, atomic writes/exact edits, and opt-in process execution.
- Bounded parallel reads, ordered receipts, and durable full results with compact model views.
- Opt-in search with source context and revision-bound multi-file edits/check recipes.
- Active-context read reuse, host check caching and optional persistent task acceptance/repair budgets.
- Content-free host/provider timing observations, including failed transport attempts.
- Embedding API, structured events, JSON-lines control interface and CLI.

See [execution semantics](docs/semantics.md), [control protocol](docs/protocol.md),
[security boundary](SECURITY.md), and [validation](docs/validation.md).
The [workflow guide](docs/workflows.md) documents the opt-in tools, checks and task API.

## An offline session

The `ah_embed` example provides an in-memory fake model and exercises the same
public lifecycle as a live provider. It is a complete embedding example, not a
replacement model or a simulation of the network provider.

## Live provider integration

An embedding host creates an `ah_responses` provider with an endpoint and a
bearer credential, then places `ah_responses_provider(provider)` in `ah_config`.
The host owns OAuth registration, refresh and credential storage. Update the token
between calls with `ah_responses_set_token`. Never give model/tool code access to
refresh credentials. Plan-authorized OAuth tokens and Platform API keys are not
interchangeable credentials; use the appropriate supported authorization flow.

Requests use the durable session UUID as `prompt_cache_key`; a fork gets its own
key. The Responses transport mirrors this key into the HTTP `session-id` header:
ChatGPT subscription routing depends on that header independently of the JSON key.
Routing is derived per request, so a shared transport cannot retain another
session's route. Custom keys must be 1–512 printable non-space ASCII bytes;
absent/null keys omit the header. This supplies stable routing, not a guarantee of cache hits.
Changes to earlier instructions, tools or history can still invalidate prefixes.

The CLI accepts `AH_ACCESS_TOKEN` or an explicitly supplied `--token-file`. A token
file must be an owner-only regular file, containing a bearer token or a SIWC JSON
record with a top-level `access_token`. Tokens are not accepted as CLI arguments.
An absent token permits offline session inspection; inference returns `auth`.

```sh
# After the host has supplied an authorized credential:
./build/ah new --db /private/path/agent.db --model MODEL_SLUG
./build/ah run --db /private/path/agent.db --session SESSION_ID \
  --workspace /approved/workspace --command-id unique-command-id \
  --prompt 'Read source.txt and summarize it.' --events
```

File writes pause for approval in the CLI. Inspect the returned pending operation,
then use `approve` or `deny` with its exact ID and run the session again. Process
execution is disabled unless `--enable-exec` is supplied, and remains approval-gated.

## Deliberate boundaries

Session state, SQLite and events have one owner; `ah_cancel` and `ah_steer` are the
cross-thread API entry points. Consecutive independent READ tools run in bounded
batches only when their registration opts in with `parallel_safe`. The default is
four workers (`max_parallel_tools`); 0 or 1 selects serial execution. Writes,
external effects and user questions remain ordered barriers. Callbacks must not
reenter owner APIs or block indefinitely. Independent
engines can run independently against different databases. One engine exclusively
owns a database; multiplex sessions through that owner.

`exec` is **not an OS sandbox**. The file tools reject symlinks and traversal, but
an approved arbitrary executable has the user's OS authority. Browser automation,
MCP servers, remote-node enrollment, mail, calendars, task-specific completion
checks and UI belong to host integrations via the tool callback contract.

Each tool receipt retains its complete captured result in the journal. Its model
view defaults to at most 16 KiB (`max_model_tool_bytes`, 0 disables the extra cap).
Larger results expose an operation reference and UTF-8 preview; `read_result`
retrieves exact pages, including in forked sessions. The view is fixed when the
receipt commits, so later configuration changes do not rewrite past prompts.
These are byte budgets, not token estimates. `read_file` defaults to 200 lines;
use its continuation cursor for complete access to long files. `edit_file` replaces
one exact match and rejects missing or ambiguous text instead of rewriting a
whole file from the model response.

Context checkpoint summaries are explicit host inputs; automatic summarization,
semantic memory and provider switching are not silently performed. Text-stream
completion is not proof of a real-world task outcome. A product should verify its
own completion conditions before presenting a task as done.

A live subscription comparison completed three synthetic tasks with both this
runtime and Codex, and exposed a fixed transport compatibility issue. Generated
evaluation outputs were removed after validation; reusable runners and graders
remain in `evals/`. This small evaluation does not establish industry leadership.
Advertised behavior is covered by executable contracts; validation boundaries
are documented.

Version 0.3 extends public C structures. Rebuild embedding clients against the new
headers and library together; it is not ABI-compatible with 0.1/0.2 objects. Source
initializers should use designated fields. Database versions 1/2 migrate transactionally to version 3;
back up persistent databases before upgrading.

MIT license. Third-party licenses and checksums are in [vendor](vendor/README.md).
