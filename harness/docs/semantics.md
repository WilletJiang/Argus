# Execution contract

## Ownership and admission

One owner thread serializes engine APIs, SQLite writes and execution. Callbacks are
synchronous and non-reentrant. `ah_cancel` sets an atomic flag; `ah_steer` copies
bounded input into a protected mailbox. Those are the only cross-thread APIs.
Independent engines may execute concurrently against separate databases.

A submitted command is durable only after its transaction commits. Reusing a
command ID with identical content is idempotent; changed content is rejected.
Steering has an explicit weaker initial acknowledgment: queued updates become
accepted only at the `input.steered` durable event. A host must retain an outbox
until it sees that event. A crash can lose an unacknowledged queued update, which
is safely resubmitted with the same ID.

## Response barrier and context

The provider consumes HTTP/SSE through a successful `response.completed` and
returns the complete native response. Text deltas are provisional observer data.
Neither syntactically complete arguments nor an intermediate function-call event
authorizes execution. Failed/incomplete/disconnected output cannot dispatch tools.

The runtime retains complete supported native output items (message, reasoning,
function_call), including opaque continuation data. It appends function outputs
with their original call IDs. Unsupported executable types and malformed required
fields fail closed. Tool schemas use the subset documented below. Tool results
are encoded as `{is_error,result}` inside the provider's function output string.

The OpenAI route emits namespaced function definitions, `store:false`, `stream:true`,
and full model-visible history. It does not emit unsupported plan-route controls
such as `previous_response_id`, `background`, or `max_output_tokens`. Authentication
scope and model availability remain the host's responsibility. Native stored
items are not claimed portable across providers.

Byte and call limits stop execution explicitly. They are not tokenizer-based
context-window prediction or a hard remote billing ceiling. Provider usage remains
available in `model.completed` records. A caller must configure model-appropriate
context limits and billing policy.

## Tool operations and authority

The runtime stores validated-model intent and snapshots the tool's schema, effect
and approval requirement. Before dispatch it validates arguments and checks the
current registration against that contract. A changed contract cannot reuse an old
approval. Approval and Ask answers bind to exact operation IDs.

States: pending, approval/input wait, approved, executing, done, unknown. An
executing intent is committed before invoking a callback. A receipt is committed
before it can enter the next model request. Call IDs cannot repeat within a
session, including retained history after compaction and forks.

A callback returning `AH_OK` with a bounded valid result provides a completion
receipt. `is_error` reports a known tool failure; `outcome_unknown` reports possible
unresolved effects. A non-read callback that fails without a trustworthy receipt
is conservatively unknown. Definitive local pre-effect failures return an error
receipt; an interrupted process execution may already have changed the world.

Missing tools and invalid arguments become explicit tool error results, allowing
the model to recover within its budgets. Runtime storage/protocol failures stop
execution rather than pretending that a tool succeeded.

## Cancellation and recovery

Cancellation prevents new dispatch and signals cooperative provider/tool code.
Built-in HTTP and process tools check it repeatedly. Already-observed effects are
not undone. Late tool receipts are saved, but cannot cause another model call.
The model/tool call counters survive stop/resume; a fresh submitted turn resets
its per-turn counters. A run deadline bounds each invocation, including waiting
inside provider/tool callbacks; arbitrary host callbacks must cooperate.

On reopen, interrupted reads receive an interrupted error result, while interrupted
writes/external operations become `outcome_unknown`. Unknown outcomes block resume
until the host reconciles with the target and calls `ah_resolve`. Model requests
interrupted by a crash are not silently repeated; the session stops for explicit
resume. Replaying events reports facts only and never invokes a provider or tool.

If writing a tool receipt fails after its execution, the persisted executing intent
is deliberately left unresolved. A later reopen takes the unknown-outcome recovery
path. This is not exactly-once execution: a crash between external effect and local
receipt cannot be resolved solely from this database.

## Store, fork and checkpoints

SQLite rollback journaling and FULL synchronization provide short transactional
state changes under one exclusive engine owner. The companion `.lock` file must
not be removed during use. Do not replace the database underneath a live engine.
Use a private local directory. FULL relies on the OS/storage durability contract;
process-kill tests do not establish power-loss behavior of every filesystem.

Fork copies an idle/completed context and historical call identities, never runnable
operations. It does not copy or undo files and external effects. Context checkpoints
are accepted only at idle/completed boundaries. They deactivate older whole turns
and insert the host's supplied summary while preserving original journal data.
They are atomic but the summary's semantic accuracy is the host's responsibility.
Schema versions newer than this runtime are rejected. Schema migration should be
performed with a backup and tested fixtures before future release changes.

## Schema subset

Tool schemas support single-string `type` (object/array/string/integer/number/
boolean/null), `properties`, `required`, boolean `additionalProperties`, `items`,
`enum`, `minLength`, `maxLength`, `minItems`, `maxItems`, `minimum`, `maximum`, and
non-executable `title`/`description` metadata. Unsupported keywords are rejected,
including in unused branches. Duplicate members, embedded NUL, excessive nesting,
nonfinite numbers and out-of-range integer lexemes are rejected. This is a
documented strict subset, not a claim to implement the full JSON Schema standard.
