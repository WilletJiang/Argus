# Subscription WebSocket experiment — 2026-11-05

**Result:** WebSocket incremental input works on the ChatGPT subscription route.
In three short paired trials it transferred 57.1% fewer JSON bytes and completed
15.3–20.4% sooner than pooled HTTP/SSE full-history requests. It did **not** show
a consistent additional latency advantage over WebSocket full-history requests.
This is evidence for the WebSocket path under these conditions, not evidence that
incremental continuation alone causes the speedup, and not a production C harness
speedup: the C provider remains HTTP/SSE.

## Question and controls

Can `store=false`, same-socket `previous_response_id` and newly added input work
with the existing subscription login, and do they improve a short tool chain over
connection-reusing HTTP? The latest official [WebSocket guide](https://developers.openai.com/api/docs/guides/websocket-mode)
describes connection-local continuation and full-history recovery. The
[SIWC limitations](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations)
restrict subscription response-ID references to the same authenticated socket;
HTTP uses full input history. These are protocol expectations, not performance
evidence. The observed performance below comes from live calls.

- Same `aiohttp` 3.13.4 implementation, public `/v1/responses` endpoint, existing
  Keychain-managed subscription identity, network and HTTPS proxy for all modes.
- Requested and returned `gpt-6.1-sol`, `xhigh`, `service_tier=default`, `store=false`
  on every successful response. Encrypted reasoning is included and replayed.
- Three synthetic tool advances with cursors 0, 1, 2, then one final response.
  Function tools use the same namespace and schema. Calls are checked before
  executing the deterministic tool. Final text must exactly match the requested
  nonce and sum. There are no external tool effects.
- Fixed reference corpus and tool-result sizes. A fresh equal-length nonce and UUID
  isolate each chain; the nonce changes a few token counts. The UUID is stable
  within the chain and supplied in both JSON `prompt_cache_key` and `session-id`.
- HTTP creates one connection per chain and observably reuses it for all nine
  continuation requests. WebSocket uses one connection per chain, without warmup
  or compression. Connection setup is measured separately. No automatic retries.
- Order: HTTP/full-WS/incremental-WS; full-WS/incremental-WS/HTTP;
  incremental-WS/HTTP/full-WS. All attempts are retained; no replacement samples.

All 36 benchmark requests reached `response.completed` with usage. Returned cache
keys matched all requested keys. The HTTP response did not echo `session-id`, and
no WebSocket handshake header echo was collected; header construction and local
tests verify sending it, not a server echo. Terminal response output arrays were
empty; completed streaming output items supplied validation and replay history.

## All three paired chains

Seconds include connection setup, four model responses, local tools and receipt
logging; connection close is excluded. The failed final answer remains in timing
statistics. Three pairs are too few for a general performance claim.

| Pair | Order | HTTP full | WS full | WS incremental | Incremental vs HTTP |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | HTTP, full, incremental | 29.244 | 15.977 | 23.291 | 20.4% shorter |
| 2 | full, incremental, HTTP | 23.210 | 17.256 | 19.582 | 15.6% shorter |
| 3 | incremental, HTTP, full | 16.343 | 19.213* | 13.844 | 15.3% shorter |
| Median | | 23.210 | 17.256 | 19.582 | |

\* WS full pair 3 executed all three correct tool calls but failed exact final-text
validation. It returned a completed response with reasoning and message output.
Raw text was intentionally not retained, so the compact record cannot distinguish
a wording/formatting deviation from an incorrect nonce or sum. HTTP and incremental
WS passed 3/3 chains each; full WS passed 2/3. Do not treat these as quality-equivalent
timings or infer that transport caused the quality difference.

Incremental WS was 45.8% and 13.5% **slower** than full WS in pairs 1 and 2, and
27.9% faster in pair 3, whose full-WS answer failed validation. Continuation-only
response sums (excluding first turn and connection setup) were respectively:
HTTP 18.067/15.730/11.661 s; full WS 9.900/11.070/10.913 s;
incremental WS 16.346/9.587/8.245 s. Removing setup therefore still does not show
an incremental advantage in every pair.

## Request-level observations

First content is a nonempty text/tool-argument delta, not response.created or
reasoning. Complete-response time ends at the terminal event; HTTP stream drain
is recorded separately. Continuation columns pool the nine later requests per mode.

| Metric | HTTP full | WS full | WS incremental |
| --- | ---: | ---: | ---: |
| Median connection setup / WS handshake (s) | 2.187 | 3.214 | 3.967 |
| Median first response completion (s) | 6.715† | 2.965 | 2.709 |
| Median continuation first content (s) | 3.996 | 3.401 | 2.829 |
| Median continuation completion (s) | 4.671 | 3.489 | 3.116 |
| Total JSON request bytes, 12 requests | 77,472 | 77,604 | 33,207 |
| Total input tokens | 31,194 | 31,230 | 31,218 |
| Cached input tokens (subset) | 8,576 | 8,576 | 12,288 |
| Output tokens | 261 | 325 | 231 |
| Reasoning tokens (subset) | 30 | 86 | 0 |

† HTTP first-response timing includes setup. WS response timing begins after its
separately reported handshake. HTTP setup ends when the pooled connection becomes
available; WS handshake includes the protocol upgrade, so these setup figures have
different endpoints and should not be subtracted as an exact transport decomposition.

First-pair JSON sizes per turn were HTTP 3,841/5,585/7,329/9,069 bytes;
full WS 3,852/5,596/7,340/9,080; incremental WS 3,852/2,407/2,407/2,403.
These exclude headers and wire framing. Incremental requests reduced upload size,
but **did not reduce reported input tokens**: prior context still counts. Different
observed cache hits and reasoning output are confounders, despite equal cache
policies and requested reasoning effort. The [conversation-state documentation](https://developers.openai.com/api/docs/guides/conversation-state)
also distinguishes response chaining from input-token accounting.

## Capability and recovery attempts

1. Initial probe failed during connection establishment (`ClientConnectorError`)
   because `aiohttp` did not apply `HTTPS_PROXY` to the `wss` URL automatically.
   No response-create request was sent. The client now explicitly uses the same
   existing HTTPS proxy for WebSocket.
2. Corrected-network probe: initial and same-socket incremental requests both
   advanced the correct tool cursor. Reusing the old ID on a new socket returned
   `invalid_request_error`; no tool was dispatched. Trying full history on that
   error socket produced no response events, so that recovery attempt failed.
3. Recovery probe: repeated same-socket continuation successfully, observed another
   old-ID rejection on a different socket, then opened a fresh socket and sent full
   history with no previous ID. Final text passed; executed-tool count stayed at two.
   The successful recovery replayed 7,336 JSON bytes and completed in 3.470 s.

The old-ID errors omitted a machine-readable error code. The retained second
error classification confirms that its message mentioned a previous response.
The test supports discarding an error socket before recovery; it does not establish
that every request error always closes a socket. An actual access-token rotation
was not forced. Disconnect recovery is demonstrated; refreshed-token recovery is
not separately measured. Durable cross-process recovery and uncertain in-flight
tool effects are also outside this small in-memory experiment.

The unchanged C `cache_driver` was compiled in a temporary Release build and made
two requests in one process: 6.987 s and 4.858 s. Both returned the requested model,
effort, tier, store value and cache key, and both produced the correct next tool
call. The driver lacks first-content and connection-counter instrumentation, so
this only confirms a functioning C HTTP baseline, not a precise C-versus-Python
comparison. The temporary build was removed.

## Complete confirmed usage

| Attempt | Responses with usage | Input | Output |
| --- | ---: | ---: | ---: |
| Initial connection failure | 0 | 0 confirmed | 0 confirmed |
| Proxy-corrected probe, including failed recovery | 2 | 3,753 | 34 |
| Fresh-socket recovery probe | 3 | 6,735 | 90 |
| Three paired benchmark groups | 36 | 93,642 | 817 |
| C sanity | 2 | 3,759 | 34 |
| **Total confirmed** | **43** | **107,889** | **975** |

Total confirmed input + output is **108,864 tokens**. Cached input **31,488** and
reasoning output **143** are already included in those totals, not added again.
Two old-ID errors and one eventless failed recovery have no usage object; their
usage is unknown rather than asserted zero. These are subscription usage records,
not a monetary bill. No Platform API key was used.

## Artifacts and limits

Reusable code and 14 offline tests are in `evals/transport/`. Tests cover pooled
HTTP connection reuse, WebSocket wire shape, terminal-output fallback, preserving
missing usage, duplicate-tool prevention, and full-history recovery. After the
experiment, review corrected the first-content fallback so an empty completed
message, function arguments or custom-tool input cannot set its timestamp. New
regression tests cover empty items, valid fallbacks and later deltas. Future receipts
record nonempty-delta counts and the first-content event source. Packaging now
declares Python >=3.11 and pins `aiohttp==3.13.4`. A subsequent review also hardened
`run.py`: a completed event followed by a transport/drain error cannot dispatch
tools; accepted responses must confirm `store=false` and a matching cache key;
and probe success now requires the cross-socket negative control to reject
the old response ID specifically, as well as successful full-history recovery.
Focused offline regressions exercise both barriers. No live inference was repeated.

The original source hashes and all measured values remain untouched in the ledgers.
Current `workload.py` still matches its benchmark hash. `client.py` differs in the
first-content validation and telemetry, and `run.py` differs in the success barriers
described above. Tests and package/dependency files were added or extended after
measurement. These changes do not rewrite the historical results or source hashes.
Raw generated content and credentials were never persisted. Existing Keychain login
is retained.

An offline audit found generated delta events in all 36 responses: five function-
argument deltas in each of 27 tool responses and 21–25 output-text deltas in each
of nine final responses. The old ledger counted event types, not nonempty delta
payloads or timestamp provenance, so it cannot prove that all 36 first-content
timestamps came from nonempty deltas. Each response has only one non-reasoning
completed item. In the 35 validated responses that item is demonstrably nonempty,
so rejecting empty fallbacks cannot change their timing condition. The remaining
failed final answer cannot be checked as strictly without its discarded raw content.
The first-content figures therefore retain this audit limitation. Complete-response
time, total-chain time, request sizes and usage do not depend on the fallback and
are unaffected by the correction.

The [consolidated ledger](websocket-evaluation.json) embeds the five original
attempt records and their original-file SHA-256 values without changing the data:
initial connection failure, first continuation/recovery attempt, fresh-socket
recovery, all paired samples and C sanity. It retains all returned usage, request
sizes, hashes, event counts and timings.

This workload is deliberately short and synthetic: three forced function calls,
small outputs, one model, one account, one client stack and one proxy route. It does
not establish behavior for long coding tasks, parallel tools, larger histories,
other providers, network failures during generation or production C scheduling.
A subsequent C implementation should preserve full local history and tool receipts,
invalidate response-ID continuation on reconnect or credential replacement, and
measure realistic task quality before adopting WebSocket as the default. No larger
or ten-minute task was run for this experiment.
