# Reducing serial turns and repeated generation

Status: design and rollout gates, 2026-11-05. The bounded phase 0–3 implementation
is now documented in [workflows](workflows.md): telemetry, optional contextual search,
versioned structured hunks, durable edit/check phases, active-context read reuse and
task acceptance with persistent budgets. The observations below explain the design;
unified/custom patch encoding and asynchronous overlap remain proposals. The
[paired smoke evaluation](evaluations/execution-evaluation.md) does not show a general
speedup, so expanded tools are opt-in. The production provider remains HTTP/SSE;
native WS is blocked by missing compiled libcurl support on the tested machine.

## Objective and evidence

Minimize wall time to an accepted result at a fixed quality level. Include context
acquisition, generation, tools, validation and repair. Track natural completion and
censored runs separately. Reduced request bytes, token usage or first feedback are
useful explanations, not sufficient evidence of faster verified completion.

The current runtime already has stable cache routing, append-only native history,
ordered durable receipts, four-way bounded read batching, paged reads, exact edits,
full-result references and cancellation/recovery. A new thread pool or a generic
planner is not the first missing component. The Sol evaluation's similar output
volume and incomplete releases instead motivate testing tool granularity,
information delivery and earlier verification. Existing records do not establish
how much generated code was repeated, so that must be measured before claiming a
specific regeneration saving.

A model boundary is necessary when the next action needs semantic interpretation
of a new observation. It is avoidable when all arguments are already known, or the
next operation is a deterministic continuation such as retrieving error excerpts.
The runtime should execute that known work without another model decision.

For example, a coding task may currently alternate between the model and host to
search paths, read matches, edit, run a known check, read the error location, repair,
and rerun that check. The target interaction is:

```
model: choose search -> host: matches + useful source excerpts
model: choose edits + check preset -> host: apply, check, attach failure excerpts
model: choose repair -> host: apply, recheck, record acceptance evidence
```

This is a proposed interaction, not a measured three-turn completion guarantee.
Missing design information still needs another model decision. A clean check does
not complete a task with missing deliverables or unresolved subjective requirements.

## 1. Task control and completion evidence

Keep a small task controller above the core session loop. Its contract contains:

- The user's objective and invariant constraints, with a contract revision.
- Authorized workspace/resources and trusted check presets supplied by the host.
- Required deliverables and observable acceptance conditions, where available.
- One total wall/model/tool/repair budget covering every continuation, not just a turn.

The model may propose milestones in its first ordinary response; a separate planning
model call is not required. Store facts about changed resources, completed checks,
remaining requirements and unresolved effects. Do not store guessed intent as proof.

A completed model turn becomes a completion candidate. The host checks acceptance
against the current workspace revision. It either records acceptance, supplies a
concise failure report for another bounded turn, or asks for genuinely missing user
input. `ah_submit` currently resets turn budgets, so an optional task controller
must retain and enforce the task-wide remaining budget across those submissions.
Core `AH_COMPLETED` continues to mean completed session turn; it must not be relabeled
as independently verified real-world success.

A check receipt binds at least:

```
check_id, check_definition_digest, contract_revision,
input_revision_set, relevant_environment_fingerprint,
status(pass/fail/error/unknown), diagnostics, duration, full_result_ref
```

A successful check for an old source revision cannot accept a new revision. Failed,
missing, unknown or infrastructure-error checks cannot become success. A model-written
test is useful evidence but does not replace independent requirements. For inherently
subjective tasks, retain the actual evidence class instead of pretending that file
existence or a model assertion proves semantic quality. Documentation existence,
changed stub bytes and executable examples are mechanical checks; prose adequacy
is a separate requirement.

The controller uses existing intent/receipt/reconciliation rules. Authorized routine
work proceeds directly; missing human facts use `ask`. Verifier output is data,
not authority to expand permissions. A full task controller API is a later explicit
extension, not hidden behavior in a provider callback or observer re-entry.

## 2. Acquire usable context in one operation

Add one bounded search tool that returns matches **with surrounding content**, rather
than only paths followed by another obligatory read. Search is scoped to an authorized
resource root, has explicit path/query limits, skips unsupported content, merges
overlapping excerpts and returns a deterministic result order. The first implementation
can reuse a trusted host search backend; implementing a new indexing engine is not a
prerequisite. Backend path handling must retain the local tools' workspace/symlink rules.

Each excerpt identifies its resource, revision, range, match reason, truncation and
continuation reference. Use literal/glob search first; regex, semantic indexing and
language-server adapters need their own measured justification. If necessary context
cannot fit, say what was omitted and how to retrieve it. Do not silently substitute
a plausible summary for source text.

For known independent targets, the model should emit multiple existing `read_file`
calls in one response. The current read scheduler already executes eligible adjacent
calls concurrently and publishes ordered results. A redundant generic batch tool is
not needed for this case. A search-plus-excerpts operation is useful because it removes
a data-dependent discovery/read boundary, not because it starts more threads.

Maintain a small index of what source ranges/revisions are actually visible in active
model context. On repeated retrieval, reuse a reference only when that content remains
visible in the current context epoch. A forced full read remains available. Compaction,
fork/context replacement and source changes invalidate the appropriate visibility
entries. References must resolve or return an explicit stale/unavailable result.

Preserve earlier model-visible receipts. Deduplicate overlapping material in a newly
created receipt; never rewrite old history in place to shrink a request. Stable
instructions/tool definitions and append-only facts preserve the current cache behavior.
A task snapshot is appended only when meaningful facts change, rather than inserting
new clocks/status text ahead of the reusable prefix each turn.

## 3. Match the edit representation to the change

Retain exact `edit_file` for a unique small replacement. Add a versioned, multi-hunk
patch operation for changes that otherwise repeat large sections or require many
small edit calls. Use `write_file` for creation and deliberate whole-file replacement.
Tool descriptions must make those choices unambiguous; extra synonymous tools are
not automatically useful.

An in-memory illustration changed one define in the existing 9,524-byte file reader:
full-write arguments were 9,984 UTF-8 JSON bytes, existing exact-edit arguments 124,
and a versioned unified patch 587. The file was not changed. This demonstrates a
representation difference, not model-token counts, live latency or a universal patch
advantage; exact replacement was smallest in this example.

Patch invariants:

- Bind edits to an observed base revision. For the current bounded editable-file
  size, compare content-derived revisions rather than trusting mtime alone.
- Parse and validate all hunks before writing. Refuse missing/ambiguous context;
  do not guess a fuzzy location. Preserve permissions and existing byte-limit rules.
- For one file, prepare the final bytes and use the existing atomic replacement path.
  Reject a stale base with current revision and concise conflict excerpts so the next
  model turn can repair it without a separate discovery round.
- Return applied/no-op/conflict/partial/unknown honestly, with changed ranges, before/
  after revisions and a durable operation reference. Successful receipts need not
  echo the entire file back to the model.

Revision checking is not a portable filesystem compare-and-swap against arbitrary
external writers. A host-owned workspace write coordinator serializes our writers;
external edits can still race and require detection/reinspection. Never describe a
check-before-rename operation as guaranteed cross-process CAS.

Start with single-file multi-hunk edits. For multi-file groups, prevalidate all files,
then journal child commit outcomes under the existing operation ID. Individual file
replacement can be atomic; the group is not globally atomic. A crash after a subset
commits produces known partial/unknown effects and requires reconciliation. Do not
blindly retry the entire group or roll back over an external edit.

Use local function-tool encoding first. OpenAI's [patch interface](https://developers.openai.com/api/docs/guides/tools-apply-patch)
provides a useful interaction pattern, but native `apply_patch_call` or custom-tool
wire formats require subscription-route probing and new core/protocol tests. The
current core supports function calls only. A compact raw patch format is a separate
optimization after the patch semantics work; it must preserve native item/call IDs.

## 4. Compose deterministic work, then validate early

Limit the first compound operations to clear recipes:

1. Search and return useful source excerpts.
2. Apply an edit group; if all intended changes are confirmed, run selected trusted
   checks once for that group. If application fails, report checks as not run.
3. On check failure, attach bounded excerpts at relevant diagnostic locations,
   with resource revisions and full-log references.

This removes turns spent asking to read a traceback location or deciding to run an
already specified check. It does not make unknown semantic branches independent.
Presets are host-owned commands with explicit inputs, outputs, deadlines and effect
scope; an arbitrary shell command does not become read-only because it is called
verification. Arguments cannot expand an existing authorization grant.

Use three validation levels: cheap structural checks, targeted checks for a completed
milestone, and the full acceptance suite at a completion candidate. Do not rerun a
large build after every small write. Verification caches require the same source/input
revisions, check definition and relevant environment, plus an appropriate freshness
policy; clock/network-dependent checks cannot be blindly reused.

The declared inputs must cover the check's dependency closure, including relevant
configuration, generated files, toolchain and environment. If that set is unknown,
use a conservative workspace/dependency fingerprint or disable reuse. A receipt
for one edited file is insufficient to cache a suite that also reads other files.

A patch-and-check group has separate application and verification outcomes. If the
patch is committed but the test times out, its receipt says so; the next action can
rerun verification without repeating the edit. Phase persistence/reconciliation is
required before claiming crash-safe compound operations. A successful check is stale
if relevant source changes during or after it. Later asynchronous checks should run
against a pinned snapshot or be discarded on revision change.

Repeated identical failures with unchanged inputs are a progress signal, not a reason
to run the same expensive command forever. Return a compact record of the unchanged
failure and missing evidence, within a bounded repair policy. Deterministic check
caching and effect deduplication are different: generic `exec` or external effects
must not be suppressed merely because their argument strings match an earlier call.

## 5. Keep result delivery informative and bounded

Tool receipts should put status, revision, counts, changed ranges and actionable
failures before bulky data. Persist full captured output using existing result
references. Align built-in result shaping with the model-view budget while preserving
pagination. The current 64 KiB read page can become a 16 KiB generic preview, causing
an additional fetch; measure that frequency before choosing smaller defaults.

Use domain-aware compact receipts where the tool knows its output structure. Keep
an honest generic fallback for arbitrary tools. Do not omit required context just
to improve token totals. Track rereads, omitted-needed-context failures and repair
cycles as regressions. Earlier receipts and opaque native reasoning remain intact.

A completed file write need not be followed by a second model-authored narration of
all changes. The host can render structured progress/acceptance receipts. The model
still communicates genuine questions through `ask`; silence alone is not completion.

## 6. Scheduling and transport are separate decisions

Preserve the current ordering: a validated successful response authorizes dispatch;
known independent reads batch; writes, approvals and uncertain effects form barriers.
An unknown `exec` footprint is a barrier. Model-declared independence is not sufficient
to override a tool's host-declared effect contract. Shared-workspace tasks need host
coordination or separate workspaces; independent databases do not isolate filesystem writes.

Only after measuring useful overlap should we consider executing safe reads before
the response terminal or running checks while the model continues. Completed-item
availability, asynchronous native tools and WebSocket continuation are different
features. The [async-tool protocol](https://developers.openai.com/api/docs/guides/async-tool-calling)
exists, but the current C callback lifetime/owner loop cannot acquire that behavior
by setting a boolean: it would need durable pending jobs, result delivery, cancellation,
join/lifetime handling and protocol compatibility on this subscription route.

The measured WebSocket experiment showed smaller uploads and faster short chains
than pooled HTTP in three pairs, but no stable incremental advantage over full-history
WebSocket. Production adoption remains gated. A C provider must key connection state
by session/cache identity and credential generation, validate the known history prefix,
and discard response-ID continuation on reconnect, auth replacement or prefix changes.
The durable journal, not a socket response ID, is the recovery source. Full-history
recovery must not replay already committed tools. Installed libcurl WebSocket/proxy
capabilities also need checking before selecting a C implementation.

## Measurement and rollout

First add content-free measurements: request/response/tool IDs; request assembly;
connection/upload; first byte versus first generated content; complete tool item;
response terminal; durable commit; tool critical path; first/targeted/full verification;
and time to acceptance. Existing event IDs and response elapsed time are a start.
Measure parallel intervals by their union/critical path rather than their sum.

For regeneration, record tool kind, submitted argument bytes, old/new resource sizes,
actual changed ranges and normalized edit outcomes. Server item-attributed token
counts may help when available; otherwise bytes are only a proxy. Never label a
textual diff as a semantic-change measure. Track duplicate read ranges, no-op/stale
edits, time to first validation, repeated failure signatures and unknown effects.

| Phase | Deliverable | Gate before broadening |
| --- | --- | --- |
| 0 | Measurements, fixed short/medium tasks, immutable acceptance checks | Accounting/revision tests; natural-completion baseline and all failures retained |
| 1 | Search-with-context, independent read grouping, informative receipts | Fewer avoidable turns without missed context or weaker quality |
| 2 | Versioned single-file patch, then journaled edit/check groups | Patch/conflict/crash tests; less generation or repair time at equal acceptance |
| 3 | Optional task acceptance controller and check reuse | No stale pass/false completion; total budgets survive continuations/restarts |
| 4 | C WebSocket prototype, raw patch encoding or asynchronous overlap | Route capability, recovery correctness and a separate measured latency gain |

Use paired workloads covering discovery, small edits to large files, multi-file
changes, deterministic data transforms and verification failures. Keep model, effort,
route, tier and input task fixed while changing one mechanism. Separate cold/warm
connection and cache conditions. A small capability cohort precedes a larger
performance study; three samples do not support a P95 or general speedup claim.
Report success rate and time-to-acceptance together, not latency only among favorable
survivors. Run fixed transcript replays separately to isolate host overhead from
changes in model behavior. Test high versus xhigh only as its own controlled factor.

Required counterexamples include stale files, overlapping hunks, duplicate calls,
partial multi-file commits, interrupted checks, source changes during validation,
compacted-out references, missing artifacts, wrong check revision, unknown tool effects,
and failed/empty model responses. A local reference predicate was exercised against
six acceptance cases, including stale source/contract revisions and missing checks;
it is a design sanity check, not an implemented task-verifier claim.

## Implementation boundaries

Keep search/patch mechanics with `src/tools/`; result shaping and context visibility
with existing core result/context modules; transport changes in `src/providers/`.
The task controller is an optional embedding layer using public session lifecycle
and receipts, with its own durable task budget. It must not mutate provider JSON
behind the journal or re-enter the owner API from an observer. Any new public structs
or stored operation kinds require an explicit ABI/schema migration and compatibility tests.

The initial coding profile should have clear search/read/edit/execute/verify roles,
selected once before the task. Existing exact edit remains useful; avoid advertising
multiple interchangeable edit tools without evidence that the model selects well.
The [tool-design guidance](https://www.anthropic.com/research/writing-tools-for-agents)
likewise emphasizes useful observations and tool selection that a human can explain.
On this [subscription route](https://developers.openai.com/siwc/token-sharing-open-source/preview-limitations),
hosted programmatic tool calling is unsupported; the finite local recipes above do
not depend on it. The plan does not introduce a general orchestration language,
an extra planning model, or benchmark-specific hardcoded acceptance rules.
