# Control protocol

`ah rpc --db PATH [--workspace ROOT]` reads one UTF-8 JSON object per line on stdin
and writes one JSON object per line on stdout. `--parallel-tools N` configures
opt-in read concurrency (default 4; 0/1 serial; maximum 16). `--model-tool-bytes N`
sets the per-result model-view budget (default 16384; 0 disables it; nonzero minimum
1024). Each input frame is at most 1 MiB.
Malformed/oversized frames are rejected; framing recovers at the next newline.
Diagnostics use stderr. The RPC mode is an implementation-specific protocol,
not a claimed JSON-RPC 2.0 implementation.

Requests contain an `id` and `method`. Responses echo `id`, include `status`, and
optionally `result` or `error`. Events contain `event`, `seq`, `session`, optional
`operation`, and `data`. `seq:0` is provisional model text, not a durable receipt.
Durable event cursors are monotonically increasing database sequence numbers.

| Method | Fields | Effect |
| --- | --- | --- |
| `create` | `model`, optional `instructions` | New session |
| `submit` | `session`, `command`, `text` | Durable, idempotent input |
| `run` | `session` | Start owner-thread execution; reply when it stops/waits/completes |
| `cancel` | none | Request active execution cancellation; not proof of undo |
| `steer` | `session`, `command`, `text` | Queue an update during a run |
| `inspect` | `session` | Snapshot including pending operation |
| `list` | none | Session snapshots |
| `resume` | `session` | Explicitly make a stopped/failed session ready |
| `decide` | `session`, `operation`, `allow` boolean | Answer a pending approval |
| `reply` | `session`, `operation`, `value` JSON | Answer Ask/external wait |
| `resolve` | `session`, `operation`, `value`, optional `is_error` | Record a reconciled uncertain outcome |
| `events` | `session`, optional `after` sequence | Replay durable facts, never actions |
| `fork` | `session` | Copy completed/idle context and stored result access into an independent session |
| `compact` | `session`, `summary`, `keep_turns` | Atomically summarize model-visible history at a settled boundary |

During `run`, only `cancel` and `steer` are accepted; other requests return `busy`.
The client can consume events while a run executes. Closing stdin or sending SIGTERM
cancels and joins active execution. SIGINT cancels the active run while keeping RPC
available. Idle RPC blocks on descriptors without a polling timer. A UI hiding its window must keep its app-level connection alive.

`steer` returning `queued:true` is not a durable acknowledgment. The owner emits
`input.steered` after committing the command at a safe model boundary. It does not
splice user text between a model's tool calls and their results. Retry a command
whose durable acknowledgment was not observed, retaining its command ID. A changed
payload using the same ID is a conflict, not a new instruction.

`compact` accepts idle/completed sessions, or stopped/failed sessions with no
pending or unresolved operations. With `keep_turns:0` on a stopped/failed session,
it preserves current-turn user messages and steering while replacing all settled
model/tool exchanges with the supplied summary. The summary must carry forward
needed results and user replies supplied through tools. It preserves the original
journal, state and call counters. Use `resume`, then `run`, to continue afterward.

Example (each line is a separate request; replace returned identifiers):

```json
{"id":1,"method":"create","model":"MODEL_SLUG","instructions":"Be concise."}
{"id":2,"method":"submit","session":"SESSION_ID","command":"user-1","text":"Read source.txt"}
{"id":3,"method":"run","session":"SESSION_ID"}
{"id":4,"method":"cancel"}
```

Persistent snapshots expose `id`, `model`, `state`, `turn`, model/tool counters and
`pending`. Pending operations expose their operation ID, provider call ID, name,
status and argument object. UI clients render their own Ask/approval controls.
Answers must always use the exact pending operation ID, never a row number.

## Model tools

Tool calls use the `local` namespace. These are model-callable tools, not RPC
methods. The runtime advertises `ask`, `wait` and `read_result`; stored calls using
the earlier `harness_ask` and `harness_wait` names remain supported.

| Tool | Arguments | Result or pause |
| --- | --- | --- |
| `ask` | `question`, optional `options` | Wait for a host `reply` with the user's answer |
| `wait` | `reason` | Wait for an explicit host `reply` |
| `read_result` | `operation_id`, optional `offset`, `limit` | Read a UTF-8 page of a captured JSON tool result available to this session |
| `read_file` | `path`, optional `offset_line` or `offset_bytes`, `max_lines`, `force` | Read a UTF-8 file page; report its revision; reuse identical active content unless forced |
| `write_file` | `path`, `content` | Replace a workspace file atomically |
| `apply_changes` | `files` containing `path`, `edits`, optional `base_revision`; optional `checks` | Unified exact edits, one or multiple hunks/files; optional named checks after successful application |
| `find` (workflow profile) | `path`, `query`, optional `glob`, `context_lines`, `max_results`, `offset`, `snapshot` | Native literal search with merged source context, revisions and bounded pagination |
| `check` (registered presets) | `checks` | Run host-fixed named checks for current inputs |
| `exec` | `argv` | Execute a process when explicitly enabled, with approval |

The filesystem and process tools require the local tool host. File paths are
workspace-relative and reject symlink traversal. `read_file` line offsets start
at 1; byte offsets start at 0 and cannot be combined with a line offset. Continue
from `next_offset_bytes` to avoid repeating or skipping partial lines. Returned
pages are also bounded by bytes and the tool-output budget. `apply_changes` rejects
missing or ambiguous matches and preserves file permissions; its change check
does not provide compare-and-swap protection against concurrent external writers.
`exec` has broad local process authority and is not a sandbox.

The default model-visible receipt limit is 16 KiB. Oversized receipts contain a
bounded preview and operation reference; `read_result` can page the original
captured JSON without rerunning the tool. Forks inherit access to these references.
Original receipts remain durable even after context compaction. A tool's own
capture limit may already have discarded output before the receipt was stored.
`read_result` defaults to byte offset 0 and a 4096-byte limit, accepts limits up to
65536, and may return less to fit the result envelope. Offsets must fall on UTF-8
boundaries. Its `content` is a slice of the stored JSON text; use `next_offset`
while `truncated` is true. A page need not be a complete JSON value on its own.

The C API is in `include/ah`. Inputs are borrowed for the call unless documented;
registration copies tool strings but borrows tool context. Provider and tool
results transfer malloc-owned strings to the runtime. Snapshot strings are freed
with `ah_free`. Observer values are borrowed only during the callback. Providers
and tool contexts must outlive their engine; close the engine before those hosts.
