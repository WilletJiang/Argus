# Subscription cache routing investigation — 2026-11-04

The C transport omitted HTTP `session-id`. On the tested ChatGPT subscription
route, a stable JSON `prompt_cache_key` alone did not preserve the returned cache
identity. Mirroring the durable key into that header made the returned key stable
and restored reuse in controlled replays and an actual native tool loop. This is
an identified transport defect with a measured fix; it does not retroactively
attribute every difference in the earlier Codex endurance comparison to this one cause.

## Mechanism and evidence

An offline capture of Codex Desktop 0.164.0 showed both `prompt_cache_key` and
`session-id` carrying the same stable thread identifier. OpenAI's
[Codex implementation](https://github.com/openai/codex/blob/main/codex-rs/core/src/client.rs)
explicitly derives ChatGPT Responses cache affinity from the session header,
independently of the JSON field. This capture used a fake credential and loopback
server, with no model inference.

The live request ledger confirms the difference. In the body-only cases, each
successful response echoed a different key even though the supplied key remained
stable. With the header, every tested response echoed the supplied key. We did
not copy Codex's originator, user-agent or unrelated telemetry headers.

The provider now derives routing from each request, so reusing its HTTP connection
across sessions cannot retain the previous session's route. The durable session
UUID survives reopen, resume and checkpoint; a fork has a distinct UUID. Null or
omitted keys omit the header. Custom keys are restricted to 1–512 printable
non-space ASCII bytes before entering HTTP headers. Loopback tests cover switching
keys, omission, the size boundary, CR/LF and non-ASCII rejection.

## Controlled trials

All live calls requested and returned `gpt-6.1-terra`, `xhigh`, default service tier,
through `https://api.openai.com/v1/responses` using the same subscription grant.
Retries were disabled. Synthetic reference records and increments were fixed;
fresh equal-length group IDs separated cases. These IDs can tokenize differently,
so actual input counts are reported rather than assumed equal.

The initial study tested two rounds of top-level instructions versus developer
input, with/without JSON keys, followed by three concurrent clients per key mode.
Including four capability/control calls, it made 52 requests: 49 completed, two
returned HTTP 503 and one HTTP 504. A failure ended its case; it was not replaced.
Input items and stable settings remained unchanged across identical/growing
requests. Changing instructions and tools was intentional in the positive controls.
Neither JSON keys nor instruction placement made repeat reuse reliable.

A second study changed only transport header construction, using a preserved
body-only binary and the fixed binary. The order reversed in round two. Each
complete case sent cold, identical, growing, growing requests:

| Case | Input tokens per request | Cached input per request |
| --- | --- | --- |
| Round 1, JSON only | unknown | HTTP 503; case ended |
| Round 1, JSON + header | 2838 / 2838 / 3555 / 4274 | 0 / 2816 / 2688 / 3456 |
| Round 2, JSON + header | 2832 / 2832 / 3549 / 4268 | 0 / 2816 / 2688 / 3456 |
| Round 2, JSON only | 2836 / 2836 / 3553 / 4272 | 0 / 0 / 0 / 0 |

The two header replay hits were 99.22% and 99.44%. Each of three subsequent
concurrent header clients also produced `0 / 2816 / 2688 / 3456` cached tokens.
Their identical-replay inputs were 2834, 2836 and 2837 tokens (99.26–99.36% hits).
New tool data explains why growing-request percentages are lower than exact replay.
The client ledger records canonical item fingerprints, not server tokenized prefixes.

`comparison_response_id` was accepted, but every returned diagnostic comparison
was `unavailable`, including positive controls. Thus it supplied no classified
miss reason or reusable-prefix estimate. The observed usage and echoed-key evidence
stand separately. [Official diagnostics guidance](https://developers.openai.com/api/docs/guides/prompt-caching/diagnostics)
also distinguishes parameter acceptance from a conclusive comparison. Reported
cache-write counts were zero; that field alone does not establish physical cache behavior.

## Actual C model/tool loop

Both variants followed four synthetic file pointers using native `read_file`,
then returned the four required markers. Every file was preserved and the final
answer was correct. Each variant used five model calls and four tool calls. Earlier
input items remained unchanged as history grew 1 → 3 → 5 → 7 → 9 items.

| Variant | Input | Cached | Uncached | Output | Wall time |
| --- | ---: | ---: | ---: | ---: | ---: |
| JSON only | 32,322 | 0 | 32,322 | 159 | 13.388 s |
| JSON + header | 32,304 | 19,072 | 13,232 | 155 | 15.148 s |

Uncached input fell 59.06% in this pair. This is not a demonstrated latency win:
the fixed run took longer. The workloads are small, tool arguments and generated
outputs differ slightly, and one pair cannot establish general performance.

Fixed-run cached tokens by call were `0 / 0 / 3456 / 6400 / 9216`. Its first input
was only 716 tokens, below the documented 1,024-token cache minimum. Later calls
introduced roughly 2,900 fresh tokens each. A 59% whole-run hit rate here is
therefore compatible with strong reuse of the prior eligible prefix; it should
not be compared directly with the earlier 96.4% Codex aggregate from a different
trajectory. See [cache guidance](https://developers.openai.com/api/docs/guides/prompt-caching).

## Complete usage and retained evidence

| Phase | Requests / completed | Input | Cached subset | Output |
| --- | ---: | ---: | ---: | ---: |
| Initial key/layout/capability study | 52 / 49 | 162,660 | 25,856 | 245 |
| Header A/B | 13 / 12 | 40,483 | 17,920 | 60 |
| Actual tool-loop pair | 10 / 10 | 64,626 | 19,072 | 314 |
| Three parallel header clients | 12 / 12 | 40,487 | 26,880 | 60 |
| Total | 87 / 83 | 308,256 | 89,728 | 679 |

Confirmed input plus output is **308,935 tokens**. Four failed requests returned
no usage, so this is a lower bound. Cached input is already included in input;
reasoning is already included in output (reported reasoning subset: zero despite
confirmed xhigh). These counts do not establish monetary or subscription-credit savings.

The [content-free ledger](cache-evaluation.json) retains all 87 attempts, response
IDs, model/settings, request fingerprints, prefix comparisons, timing, usage and
failures. Source hashes identify the frozen controlled study. The body-only build
is commit `ba98a1c`; header construction is commit `79ff935`. Raw workspaces,
transcripts, temporary scripts, copied sources and builds are removed after recording
this report. Reusable tooling remains in `evals/`; see its README for reproduction.

Persistent OAuth is retained in macOS Keychain. A new process successfully reused
the saved session without browser login. Rotation, cross-process serialization,
invalid grants and transient failure handling have offline tests; live refresh
was not forced before the provider's refresh window. The C core has no Python or
Keychain dependency. Local ASan/UBSan passed all 18 suites after the transport fix.
