# ah — a standalone C agent harness

A C17 library and headless executable for durable tool-using agents. It runs
without Argus, Swift, AppKit, a browser UI, or an agent framework. Argus is a
prospective client, not a dependency. The project is independently buildable and
installable so it can move to its own repository without an architectural rewrite.

The supported model protocol is OpenAI Responses over HTTP/SSE, including the
ChatGPT-plan request shape. Authentication is supplied by the embedding host;
this library does not read Codex credentials or implement an OAuth browser flow.

## Build and verify

Requires CMake 3.20+, a C17 compiler, libcurl 7.68+ built with asynchronous DNS,
SQLite 3, and pthreads. Python 3 is used only by integration tests. JSON parsing
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

## What is implemented

- Persistent sessions and an ordered semantic event journal; idempotent user commands.
- A complete model/tool loop with successful-response barrier, native response items,
  bounded context/output, deadlines, and model/tool call budgets.
- Durable, operation-bound approval, Ask and external-wait requests; explicit answers.
- Cancellation and bounded in-run steering, with distinct queued/durable acknowledgments.
- Crash recovery that preserves uncertain effects; explicit reconciliation rather than
  blind re-execution of writes.
- Completed-session forks and atomic, explicit context checkpoints retaining whole turns.
- Streaming HTTPS with connection reuse, bounded parser buffers, strict UTF-8/JSON,
  conservative retries, and interruption during stalled network I/O or backoff.
- Scoped UTF-8 file reads, directory listing, atomic file writes, and opt-in process execution.
- Embedding API, structured events, JSON-lines control interface and CLI.

See [execution semantics](docs/semantics.md), [control protocol](docs/protocol.md),
[security boundary](SECURITY.md), and [validation](docs/validation.md).

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

The library is single-owner; `ah_cancel` and `ah_steer` are the cross-thread entry
points. Callbacks must not reenter owner APIs or block indefinitely. Independent
engines can run independently against different databases. One engine exclusively
owns a database; multiplex sessions through that owner.

`exec` is **not an OS sandbox**. The file tools reject symlinks and traversal, but
an approved arbitrary executable has the user's OS authority. Browser automation,
MCP servers, remote-node enrollment, mail, calendars, task-specific completion
checks and UI belong to host integrations via the tool callback contract.

Context checkpoint summaries are explicit host inputs; automatic summarization,
semantic memory and provider switching are not silently performed. Text-stream
completion is not proof of a real-world task outcome. A product should verify its
own completion conditions before presenting a task as done.

A [live subscription comparison](evals/results/2026-11-03.md) completed three synthetic
tasks with both this runtime and Codex, and exposed a fixed transport compatibility
issue. No claim of industry leadership is made from this small evaluation. Advertised behavior is covered by executable contracts, and the
remaining validation boundaries are documented.

MIT license. Third-party licenses and checksums are in [vendor](vendor/README.md).
