"""Frozen multi-component task. The agent sees SPEC and smoke tests, never the grader."""

from pathlib import Path
import hashlib

PROMPT = """Finish the workboard package according to SPEC.md. This is a complete release task: implement every documented API and CLI behavior, add focused regression tests, run verification, and document usage and any remaining limitations in README.md. Preserve SPEC.md and the supplied smoke tests. Work independently through the entire backlog, not just one milestone. Do not ask for clarification when the specification already determines the behavior. Use only the Python standard library, no network, and only files inside this workspace. Python is available at /usr/bin/python3. Return a concise factual completion report after verification."""

SPEC = r"""# Workboard 1.0 release contract

Build a durable local work queue for a personal assistant. It must support restart,
atomic batch updates, dependency-aware claiming, lease ownership, querying, and a
JSON-lines CLI. Python 3.9+ standard library only. No database/queue library or
sqlite3; persistent state must be human-readable JSON. Design small modules rather
than putting everything in one file. Single Store owner per directory is sufficient;
concurrent processes and power-loss guarantees beyond atomic file replacement are
not required. All supplied inputs and outputs below are part of the public contract.

## 1. Public API and validation

`from workboard import Store, ValidationError, Conflict, NotFound` must work.
The three exceptions subclass ValueError, RuntimeError, and KeyError respectively.
`Store(directory)` creates/reopens state, implements `close()` (idempotent), and is
a context manager. Calls after close raise RuntimeError. The directory can already
exist. Never reset or silently replace corrupt durable data; reopen raises
ValidationError. Every successful mutating call persists before returning.

IDs and worker IDs are nonempty strings without whitespace. Priority is any Python
int except bool. max_attempts is an int (not bool) >=1. Timestamps, lease durations,
and retry delays are finite int/float except bool. Durations >0, retry delay >=0;
`now` and `available_at` may be negative. Payloads/results are any JSON values:
reject NaN/Infinity, non-string object keys, tuples, and arbitrary objects recursively.
Inputs and returned records must be deep-copy isolated. Validation failures must
leave state, journal and idempotency records unchanged.

A job record has exactly these public fields:
`id, payload, depends_on, priority, max_attempts, available_at, state, attempts,
lease_token, lease_until, worker, result, error, sequence`.
States are `queued`, `running`, `succeeded`, `failed`, `cancelled`. Creation gives
attempts=0, lease fields and worker/result/error=None. Depends_on is a sorted unique
list. sequence is the monotonically increasing creation number, starting at1;
failed/rolled-back creates must not consume it. Creation order breaks priority ties.
`get(id)` raises NotFound when absent; `list_jobs(state=None)` returns records in
creation order, optionally filtered by a valid state. Unknown state ->ValidationError.

## 2. Creation and idempotency

`create(id, payload, *, depends_on=(), priority=0, max_attempts=3,
available_at=0, idempotency_key=None)` returns the new record. All prerequisites
must already exist; unknown dependency ->NotFound; self-dependency ->ValidationError.
Duplicate ID ->Conflict. idempotency_key is None or a nonempty string. Reusing the
same key with semantically identical creation parameters returns the current record
without creating another event, even after completion/reopen. Normalize dependency
order and duplication for this comparison. Reusing a key with any different
parameter ->Conflict, including a different job ID. Failed validation must not reserve
an idempotency key. Distinct new keys cannot bypass a duplicate ID.

`set_dependencies(id, dependencies)` only accepts queued jobs, validates known IDs
and rejects any direct/indirect cycle atomically. Unknown job/dependency ->NotFound;
cycle/self ->ValidationError; nonqueued target ->Conflict. Existing failed/cancelled
prerequisites are allowed but block claiming until they eventually succeed.

## 3. Claiming and leases

`claim(worker, now, lease_seconds)` first validates its inputs, then reaps all expired
running jobs at `now >= lease_until`. An expired job becomes queued (available_at=now)
if attempts < max_attempts; otherwise failed. Set error="lease expired" and clear
all lease/worker fields. Reaping happens even if nothing can be claimed.

Among queued jobs with available_at<=now whose dependencies all succeeded, claim
highest priority then lowest creation sequence. Return None if none eligible.
A claim increments attempts, sets running, worker, a fresh nonempty lease_token and
lease_until=now+lease_seconds. Never reuse a lease token, including after reopen;
clear error on a new claim. A token is ownership, not just an informational ID.

`heartbeat(id, token, now, lease_seconds)` extends an owned live running lease to
max(current lease_until, now+lease_seconds); never shortens it. Returns the record.
`complete(id, token, result, now)` succeeds the job and sets result.
`fail(id, token, error, now, retry_delay=0)` requires a string error; queues it at
now+retry_delay if attempts < max_attempts, otherwise fails it. It returns the record.
All three require matching token and a still-live lease (now<lease_until), otherwise
Conflict; these methods do not themselves reap expired jobs. Unknown ID ->NotFound.
Completion/failure clear token, worker and lease_until. No errors may partly mutate.

`cancel(id)` cancels queued/running jobs and clears leases; cancelling an already
cancelled job is idempotent with no event. Succeeded/failed ->Conflict.
`retry(id, *, available_at=0)` only accepts failed/cancelled jobs, resets attempts to0,
state toqueued, result/error/lease/worker toNone, retaining creation sequence and
prerequisites. Other states ->Conflict. A succeeded prerequisite remains succeeded.

## 4. Atomic transactions and durable events

`with store.transaction(): ...` groups arbitrary mutations into one atomic commit.
Reads inside see staged state. If the block raises, roll back jobs, sequence,
idempotency keys and events and propagate the exception. The on-disk state stays
unchanged until commit. Nested transactions ->Conflict without breaking the outer
transaction when caught. Closing while in a transaction ->Conflict. A separate
Store opened on the same directory while a transaction is staged sees the previous
committed state; tests will only read that second owner.

`events(after=0)` returns deep copies of events with event seq>after. after must be
an int>=0 excluding bool. Event seq starts at1, no gaps after rollback or failed calls.
Each event is exactly {seq,kind,job_id,record}, where record is the resulting full
job snapshot. Kinds: created, dependencies_changed, claimed, heartbeat, completed,
retry_scheduled (nonterminal fail), failed (terminal fail), lease_expired, cancelled,
retried. Each reaped job emits lease_expired even when it becomes failed. Reap in
creation order, then emit any claim. An unchanged set_dependencies or heartbeat
may emit its named event; create idempotency and repeated cancel must not emit.
Events survive reopen, and old event snapshots never reflect subsequent mutation.

`compact()` must preserve jobs, complete event history, sequence and idempotency.
It must leave a valid durable store, never invalidate an active lease, and be
idempotent; inside a transaction ->Conflict. A real serialized snapshot/rewrite is
sufficient, append-only WAL design is optional. Temporary write files must be
cleaned after a successful operation. `export(path)` writes deterministic JSON
containing {"format":1,"jobs":[...],"events":[...]}; these public lists match current
state, including staged state inside a transaction. No extra top-level fields.
Use sorted JSON keys, ensure_ascii=False, separators=(',',':'), one trailing newline.
Exporting twice with unchanged state produces byte-identical output.

## 5. Pure analytical query engine

`from workboard.query import query, matches, join, aggregate`.
`matches(record, expression)` evaluates a JSON expression. `None` matches everything.
Logical forms: {"and":[expressions]}, {"or":[expressions]}, {"not":expression};
empty and=True, empty or=False. Predicate form exactly {"field": dotted.path,
"op": operator, "value": JSON value}; for exists use a boolean value. Supported
operators: eq,ne,lt,lte,gt,gte,in,contains,prefix,exists. Missing dotted fields use
None for eq/ne/in and do not satisfy ordered comparisons/contains/prefix. exists
checks actual presence, including present None. Ordered comparisons only work for
number-number (excluding bool) or string-string; incomparable values return False.
`in` requires list value. contains supports list membership or string substring
(string needle only). prefix requires string value, and tests str.startswith.
Invalid shapes/operators/types raise ValidationError, even in an unreachable
logical branch: validate the whole expression before evaluation. Dotted paths may
traverse only objects, not lists; empty/non-string path ->ValidationError.

`query(records, *, where=None, fields=None, order_by=(), offset=0, limit=None)`
returns deep copies. records may be a generator; do not mutate inputs. Filter first,
then stable sort, then slice, then project. order_by consists of dotted paths;
leading '-' means descending. Missing/None sort last in either direction. Values
within a sort field are either homogeneous comparable scalars, or missing/None;
invalid order fields/types ->ValidationError. Ties preserve original input order.
Projection fields is None (full records) or a list of dotted paths; projected output
keys are the original dotted paths and missing values become None. offset/limit are
nonnegative ints excluding bool; None limit means unbounded.

`join(left, right, *, left_on, right_on, how='inner', prefix='right.')` performs a
stable equijoin. left order first, right order second. how is inner or left only.
Missing/None keys never match. For each match copy the left record and add every
right top-level key under prefix+key; collision with a left key ->ValidationError.
Left-join unmatched rows add all right schema keys (union of keys across right rows)
with None values. Inputs may be generators and must not be mutated. Join keys are
scalars; invalid parameters ->ValidationError. Empty right left-join returns left
copies unchanged (empty schema).

`aggregate(records, *, group_by=(), metrics)` returns groups in first-appearance
order. Group keys are dotted fields, missing->None; output group keys use dotted
names. metrics maps output name to {"op": count|sum|min|max|avg, "field": path}.
count may omit field to count rows; with field it counts non-None values. Other
operations ignore None; sum(empty)=0, min/max/avg(empty)=None. Numeric aggregates
reject nonnumeric/bool input. No groups+empty records returns one aggregate row;
nonempty group_by+empty records returns []. Group/output name collisions, malformed
specifications, invalid paths, non-scalar group keys ->ValidationError. Never mutate.

## 6. Dependency planning and snapshots

`from workboard.planning import plan, blocked`.
`plan(jobs)` returns a list of layers (lists of job IDs). Topological order over all
provided jobs, each layer sorted by creation sequence. Dependencies already in an
earlier layer are satisfied regardless of job state. Unknown dependencies ->NotFound,
cycles/duplicate IDs ->ValidationError, empty input ->[]. Do not mutate inputs.
`blocked(jobs)` returns a dict for every queued job whose prerequisites are not all
succeeded, ordered by creation sequence: ID -> sorted IDs of its unsucceeded direct
prerequisites. Unknown dependencies ->NotFound. Other job states are omitted.

## 7. CLI and release completeness

`python -m workboard --db DIRECTORY` reads JSON objects one per stdin line and emits
exactly one compact JSON reply per line, no banners/logging on stdout. Commands:
{"op":"create","id":...,"payload":..., optional create keyword arguments};
{"op":"get","id":...}; {"op":"list", optional "state":...};
{"op":"claim","worker":...,"now":...,"lease_seconds":...};
{"op":"complete","id":...,"token":...,"result":...,"now":...};
{"op":"fail", ...same fail arguments...}; {"op":"heartbeat", ...};
{"op":"cancel","id":...}; {"op":"retry","id":..., optional "available_at":...};
{"op":"events", optional "after":...}; {"op":"compact"};
{"op":"query", optional query keyword arguments}, running against list_jobs();
{"op":"plan"}; {"op":"blocked"};
{"op":"batch","commands":[mutating commands create/set_dependencies/cancel/retry]}.
Batch is atomic and returns ordered per-command results; invalid/failed command
rolls back the entire batch. set_dependencies accepts id and dependencies.
Replies: {"ok":true,"result":value} or {"ok":false,"error":{"type":exception
class name,"message":nonempty string}}. Malformed JSON/unknown operation/unknown
fields ->ValidationError, and processing continues on the next input line. Store
exceptions keep their public class names. EOF exits0; paths supplied to --db work
outside the current directory. Document command examples and architecture, and add
regression tests for error handling, restart, transactions, queries and leases.
"""

SMOKE = r"""import tempfile, unittest
from workboard import Store, Conflict
from workboard.query import query, aggregate
from workboard.planning import plan
class Smoke(unittest.TestCase):
    def test_claim(self):
        with tempfile.TemporaryDirectory() as d, Store(d) as s:
            s.create('a', {'x':1}); s.create('b', None, depends_on=['a'], priority=8)
            j=s.claim('w',0,10); self.assertEqual(j['id'],'a')
            s.complete('a',j['lease_token'],{'ok':True},1)
            self.assertEqual(s.claim('w',1,10)['id'],'b')
    def test_transaction(self):
        with tempfile.TemporaryDirectory() as d, Store(d) as s:
            try:
                with s.transaction():
                    s.create('a',None); raise RuntimeError('abort')
            except RuntimeError: pass
            self.assertEqual(s.list_jobs(),[])
    def test_query(self):
        rows=[{'x':2},{'x':None},{'x':1}]
        self.assertEqual(query(rows,order_by=['-x'],fields=['x']),[{'x':2},{'x':1},{'x':None}])
        self.assertEqual(aggregate([{'x':2},{'x':4}],metrics={'a':{'op':'avg','field':'x'}}),[{'a':3}])
    def test_plan(self):
        self.assertEqual(plan([{'id':'b','sequence':2,'depends_on':['a']},{'id':'a','sequence':1,'depends_on':[]}]),[['a'],['b']])
if __name__=='__main__': unittest.main()
"""

SKELETON = """class ValidationError(ValueError): pass
class Conflict(RuntimeError): pass
class NotFound(KeyError): pass

class Store:
    def __init__(self, directory): raise NotImplementedError('Implement SPEC.md')
"""


def prepare(workspace):
    workspace = Path(workspace)
    workspace.mkdir(parents=True, exist_ok=False)
    (workspace / "workboard").mkdir()
    (workspace / "tests").mkdir()
    (workspace / "SPEC.md").write_text(SPEC)
    (workspace / "workboard/__init__.py").write_text(SKELETON)
    (workspace / "workboard/query.py").write_text(
        "def matches(record, expression): raise NotImplementedError\ndef query(records, **kwargs): raise NotImplementedError\ndef join(left, right, **kwargs): raise NotImplementedError\ndef aggregate(records, **kwargs): raise NotImplementedError\n"
    )
    (workspace / "workboard/planning.py").write_text(
        "def plan(jobs): raise NotImplementedError\ndef blocked(jobs): raise NotImplementedError\n"
    )
    (workspace / "tests/test_smoke.py").write_text(SMOKE)
    (workspace / "README.md").write_text(
        "# Workboard\n\nImplementation and release documentation pending.\n"
    )
    return PROMPT


def protected_hashes(workspace):
    return {
        p: hashlib.sha256((Path(workspace) / p).read_bytes()).hexdigest()
        for p in ("SPEC.md", "tests/test_smoke.py")
    }
