# Performance investigation

The objective is **less wall time to an independently accepted result at the same
quality**, including verification and necessary repair. Total tokens, local loop
speed and first visible feedback are supporting metrics, not substitutes.

## Evidence so far

The Sol comparison produced 77,548 harness output tokens versus 77,008 for Codex:
only a 0.7% difference. Both cohorts reached the ten-minute cutoff. Input volume
was lower for the harness, but external skills, plugins and memory were already
disabled for Codex. Built-in instructions, tool schemas, context policies and
trajectories still differed. This does not establish an intrinsic runtime advantage.

The harness recorded 12/11/13 completed model responses; Codex recorded 9/9/9
changes to cumulative response usage. These suggest investigating unnecessary
serial turns, but are not interchangeable network round-trip measurements.
[Full evidence](evaluations/sol-xhigh-evaluation.md).

Local Release measurements after the layout cleanup (three repetitions of 100
turns) put SQLite/FULL P50 at 1.354–1.728 ms with short history, and 2.313–2.351 ms
with a retained 1 MiB user item. The synthetic provider does no inference, networking
or real tool work. These measurements support keeping the C loop small; they do
not profile the entire live transport or establish a refactor speedup. See
[measurement details](evaluations/local-performance.md).

## First: separate the time components

Before choosing an optimization, record a monotonic timeline for request assembly,
connection/upload, first server byte, first generated content, each completed tool
item, response completion, durable commit, tool execution and verification. A
response-created event is not the first generated token. Parallel tool intervals
must be combined as a critical path, not blindly summed.

Existing evaluation traces preserve response elapsed time, session/operation IDs
and durable event timestamps. They do not yet separate first-byte/prefill/generation
time. Keep any added telemetry content-free and test its overhead independently.
Include model, effort, route, returned tier, errors, uncached input and output usage.

The next performance suite should mix short search/edit tasks, medium multi-file
changes and a small number of long tasks. Use fixed acceptance checks and enough
time for natural completion; retain censored runs separately. Change one mechanism
at a time and compare paired task success, median/P95 time and repair cycles.

## Highest-value experiments

| Question | Candidate mechanism | Convincing evidence |
| --- | --- | --- |
| Can the model get the required context with fewer turns? | Bounded search/context retrieval; group independent reads and return focused snippets with stable references | Fewer serial calls, equal acceptance scores, shorter critical path |
| Can changes be expressed with less generation? | Prefer existing exact edits for local changes; compare a model-native patch interface before implementing it | Less regenerated code/output and less wall time without more edit failures |
| Can validation catch a wrong assumption earlier? | Host-declared acceptance milestones; run targeted checks after a meaningful change and return concise failures | Earlier first verification, fewer repeated repairs and more fully accepted tasks |
| Is persistent incremental transport worthwhile on this route? | WebSocket continuation on one authenticated connection, sending new items after the prior response | Lower per-turn transport/continuation latency with correct reconnect, refresh and replay behavior |
| Is xhigh worth its latency for every task? | Controlled high-versus-xhigh comparisons by task class, escalating when evidence requires it | Equal quality at lower latency, counting escalation and failed attempts |

Output generation is often the dominant component; reducing input alone need not
reduce wall time proportionally. This is also the direction of OpenAI's
[latency guidance](https://developers.openai.com/api/docs/guides/latency-optimization).
The current data does not tell us how much time each mechanism would save here.

The HTTP provider already reuses libcurl connections. A WebSocket experiment
should isolate incremental history and continuation overhead from ordinary
connection reuse. WebSockets deserve a bounded prototype, not an assumed speed claim. The
[Responses transport](https://developers.openai.com/api/docs/guides/websocket-mode)
offers persistent, incremental continuation. On the
[subscription route](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations),
HTTP still requires full history; WebSocket continuation only references responses
from the same authenticated connection. The durable journal must remain sufficient
to rebuild context after disconnect or token renewal. Tool calls must never be
replayed blindly after uncertain effects.

## Later, only if the timeline justifies it

Executing explicitly safe local reads after their completed item arrives could
overlap work with remaining generation. Results would stay provisional until a
successful response terminal, with bounded cancellation/draining. Measure the
available overlap window first: warm local reads may be too short to justify the
extra lifecycle complexity. Writes retain their durable intent and approval order.

Keep native full-history reconstruction and allocator/SQLite optimizations behind
measured profiles. Consider incremental serialization or prepared statements only
if representative long-context cases show meaningful host time. Current microsecond
file callbacks and millisecond mock loops do not justify treating these as the
primary live bottleneck.

No live optimization comparison was run as part of the layout cleanup. The changes
establish clearer ownership, shared evaluators and reproducible local measurements
for the next experiments; they do not claim faster model inference.
