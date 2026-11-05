# Subscription transport experiment

This is a protocol experiment with the same Python `aiohttp` stack on both sides.
It does not add WebSocket support to the production C provider. The endpoint is
fixed to the public Responses API; authentication comes only from `AH_ACCESS_TOKEN`.
Requires Python >=3.11 (`asyncio.timeout`) and the optional `aiohttp` dependency,
pinned to the measured version 3.13.4 in `requirements.txt`.

From the harness root, after the existing subscription login is available:

```sh
python3 -m pip install -r evals/transport/requirements.txt
python3 -B -m unittest discover -s evals/transport -t .
python3 -B -m host.auth exec -- python3 -B -m evals.transport.run \
  --probe --output /private/fresh-websocket-probe.json
python3 -B -m host.auth exec -- python3 -B -m evals.transport.run \
  --rounds 3 --output /private/fresh-websocket-comparison.json
```

Run the capability probe first. It checks same-socket incremental tool continuation,
deliberately rejects an old response ID on a new socket, then opens another socket
and recovers from complete history. The deliberately invalid request never dispatches
tools. Recovery preserves the already executed tool ledger. No real side effects
occur: the tool returns deterministic synthetic records.

The 14 offline tests cover stream parsing, connection reuse, content timing and
success barriers. A terminal completed event followed by a transport/drain error
cannot dispatch tools. Accepted responses must also confirm `store=false` and a
matching cache key. The capability probe passes only when the cross-socket
negative control specifically rejects the old response ID and recovery succeeds;
an unexpected success or unrelated error is a failed probe.

The comparison runs three modes in balanced cyclic order: HTTP full history,
WebSocket full history, and WebSocket incremental input. Each chain contains three
tool calls followed by an exact final answer. Each starts with a fresh equal-length
nonce and cache key. HTTP uses one pooled connection for the chain; WebSocket uses
one authenticated connection. The key is also sent as `session-id`. Both transports
use the existing HTTPS proxy, if configured. There are no automatic retries.

Receipts retain request byte counts, fingerprints, event counts, model/effort/tier,
exact validation results, timing and the entire returned usage object. Raw prompts,
response text, reasoning, bearer tokens and tool results are not written. A missing
usage object stays unknown. Cached tokens are a subset of input; reasoning tokens
are a subset of output. Output items collected during the stream are used when the
terminal response has an empty output array.

`first_content_ms` starts at request serialization completion and stops at the first
nonempty output text or tool-argument delta (or a nonempty completed text, function
arguments or custom-tool input item if deltas are absent). It excludes empty items
and created/in-progress/reasoning-only events. New receipts also record the number
of nonempty generated deltas and which event supplied the first-content timestamp.
`completed_ms` ends
at the terminal event; `elapsed_ms` also includes draining the HTTP stream. WebSocket
handshake cost is separate; HTTP's first request includes connection establishment.
`chain_ms` includes connection setup, all requests, local synthetic tools and receipt
logging, and excludes connection close. Request bytes exclude transport framing and
headers. Compare later turns separately from initial requests.

For two additional HTTP checks through the unchanged C transport:

```sh
cmake -S . -B /private/fresh-build -DAH_LIVE_EVAL=ON \
  -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /private/fresh-build --target cache_driver --parallel
python3 -B -m host.auth exec -- python3 -B -m evals.transport.c_sanity \
  --driver /private/fresh-build/cache_driver --output /private/fresh-c-sanity.json
```

The C driver stays alive for both calls. It exposes complete-response timing, but
does not expose first-content timing or observed connection-reuse counters. This
sanity check is not a C-versus-Python performance comparison. Remove the temporary
build afterward; preserve the subscription login.

The [2026-11-05 report](../../docs/evaluations/websocket-evaluation.md) includes all
measured attempts, the quality failure and the limits on interpreting the results.
