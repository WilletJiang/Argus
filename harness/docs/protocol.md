# Control protocol

`ah rpc --db PATH [--workspace ROOT]` reads one UTF-8 JSON object per line on stdin
and writes one JSON object per line on stdout. Each input frame is at most 1 MiB.
Malformed/oversized frames are rejected; framing recovers at the next newline.
Diagnostics use stderr. The RPC mode is an implementation-specific v0.1 protocol,
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
| `fork` | `session` | Copy a completed/idle context into an independent session |
| `compact` | `session`, `summary`, `keep_turns` | Atomically replace model-visible prefix |

During `run`, only `cancel` and `steer` are accepted; other requests return `busy`.
The client can consume events while a run executes. Closing stdin or sending SIGTERM
cancels and joins active execution. SIGINT cancels the active run while keeping RPC
available. Idle RPC blocks on descriptors without a polling timer. A UI hiding its window must keep its app-level connection alive.

`steer` returning `queued:true` is not a durable acknowledgment. The owner emits
`input.steered` after committing the command at a safe model boundary. It does not
splice user text between a model's tool calls and their results. Retry a command
whose durable acknowledgment was not observed, retaining its command ID. A changed
payload using the same ID is a conflict, not a new instruction.

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

The C API is in `include/ah`. Inputs are borrowed for the call unless documented;
registration copies tool strings but borrows tool context. Provider and tool
results transfer malloc-owned strings to the runtime. Snapshot strings are freed
with `ah_free`. Observer values are borrowed only during the callback. Providers
and tool contexts must outlive their engine; close the engine before those hosts.
