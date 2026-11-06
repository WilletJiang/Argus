# Workflow profile smoke comparison — 2026-11-05

**Result:** both versions passed every independent task check. The expanded profile
was faster on intervals and slower on the other two tasks; no general speedup is
established. Consequently expanded tools are opt-in, not the default profile.

The existing reconcile/intervals/update prompts and graders were unchanged. Each
task had one old/new pair run concurrently, with tasks run sequentially. Baseline
was commit `3ae5824` (runtime 0.2.2); candidate was the in-progress 0.3 implementation
with search/apply tools enabled. Both were native Release builds using the same
subscription route, model **gpt-6.1-sol**, **xhigh**, host and executor. Returned model
and effort matched in every run. All six completed naturally below the external
300-second ceiling; no failures or reruns were discarded.

| Task | Baseline seconds | Candidate seconds | Model responses old/new | Independent checks |
| --- | ---: | ---: | ---: | --- |
| reconcile | 53.091 | 57.489 | 4 / 4 | both pass |
| intervals | 91.579 | 76.399 | 6 / 5 | both pass |
| update | 35.539 | 51.553 | 4 / 5 | both pass |
| Sum across these tasks | 180.209 | 185.441 | 14 / 14 | 6/6 pass |

The candidate interval trajectory used `edit_file`, while baseline used `write_file`
and one extra directory listing. For update, candidate successfully applied one
versioned `apply_changes` group to **three files**, but took an extra model response;
baseline bundled its work into shell calls. Neither candidate selected `search` in
this small cohort. This is an important negative observation: providing finer native
tools does not ensure the model chooses a shorter trajectory. Different reasoning,
cache hits and tool choices prevent attributing a timing difference to one mechanism.

| Reported usage | Baseline | Candidate |
| --- | ---: | ---: |
| Input tokens | 25,616 | 33,976 |
| Cached input (subset) | 8,576 | 20,992 |
| Output tokens | 4,883 | 5,707 |
| Reasoning output (subset) | 671 | 802 |
| Input + output | 30,499 | 39,683 |

Total confirmed usage is **70,182 tokens**, over 28 completed model responses. No
interrupted calls occurred. Cache/reasoning subsets are not added twice. The study
used the existing persistent Keychain login, including automatic refresh; it did
not create a new API key or authorization flow.

## What the new measurements show

Candidate model-call intervals sum to 54.961, 76.118 and 51.199 seconds respectively.
Request assembly sums to **0.623 / 0.555 / 0.564 milliseconds**. Durable commits sum
to **11.048 / 12.621 / 16.878 milliseconds**. Tool interval sums are
171.021 / 226.477 / 302.836 milliseconds. A November 6 audit found that each
cancellation probe in a parallel read reset its recorded start. Reported tool sums
can consequently undercount batched reads. The wall and model durations were measured independently. The original ledger retains the emitted values. They do show that local request assembly
is not the primary measured wait in these examples.

Every candidate transport receipt reports a successful HTTP response and ordered
nonempty-content/terminal/elapsed timings. Failed/cancelled/late-stream paths were
tested offline, not inferred from these successful live calls. The baseline binary
does not contain the new phase instrumentation; wall time, usage and tool actions
are the comparable measurements.

## Native transport gate

After the live cohort, a local Release microbenchmark checked SHA-256 overhead.
With 100 measurements and 20 warmups per case, switching the new revision hashing
from portable C to macOS CommonCrypto changed P50 read-4-KiB from 70 to 51 µs,
read-64-KiB from 415 to 162 µs, and atomic-write-4-KiB from 169 to 174 µs. This is one
sample per backend, not a task-speed claim or comparison against 0.2. Both backends
matched Python hashlib on 12 boundary/large inputs. CommonCrypto is supplied by
libSystem; other platforms keep the dependency-free implementation. These later
local measurements do not replace the recorded live-build timings.

A separately compiled C capability probe checked the installed Apple libcurl 8.7.1:
headers and runtime match; asynchronous DNS and HTTPS-proxy are present, but advertised
protocols contain neither `ws` nor `wss`. Direct and explicit local-proxy WS/WSS
requests all return `CURLE_UNSUPPORTED_PROTOCOL` (1), HTTP status 0; exported
`curl_ws_send`/`curl_ws_recv` return `CURLE_NOT_BUILT_IN` (4). Exported symbols alone
therefore do not make this a usable WebSocket runtime. No bespoke WS stack or new
dependency was added. The earlier [Python transport study](websocket-evaluation.md)
remains separate; production C still uses HTTP/SSE.

## Limits and retained evidence

The [ledger](execution-evaluation.json) retains binary/source hashes, prompt hashes,
all run results, returned usage, tool names/argument byte counts and candidate phase
timings. Post-measurement changes make profile selection explicit, improve tests
and documentation, and harden task/observation edges. Recorded hashes describe the
measured build; they are not relabeled as a measurement of later edits.

Host check presets and the optional task controller were **not enabled in this live
cohort**. Their revision, caching, crash/reconciliation and repair/budget behavior
is covered by the offline workflow/task suites. This cohort cannot establish their
live speed or quality effect. It also does not establish long-task, tail-latency,
multi-account or provider-general results. Future studies should independently vary
tool profile, host checks and acceptance control, include larger discovery/edit tasks,
and retain all failures with natural time-to-acceptance.

Generated workspaces, databases, raw traces, temporary baseline checkout/builds and
capability probes are removed after compact evidence is recorded. Persistent login
is retained. Reproduce the task comparison with the unchanged `evals.smoke` cases
and the shared harness runner against the two recorded native builds.
