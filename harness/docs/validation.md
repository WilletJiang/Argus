# Validation and release gates

All default tests are local and deterministic. They do not contact OpenAI or
consume subscription/API quotas. Tests create temporary state and clean it up.

| Suite | Coverage |
| --- | --- |
| core | Native tool loop, duplicate commands/call IDs, approval/input resume, fork/checkpoint, cancellation/steering, process-crash recovery, receipt-write failure |
| alloc | Per-document allocation budgets, sticky builder failure, serialization/realloc failure, no partial JSON |
| stream | Every fragmentation width, UTF-8/CRLF/BOM, duplicate/oversized/deep JSON, terminal barriers |
| stream_items | Completed item reconstruction when terminal output is empty; out-of-order indices, gaps, duplicate/conflicting identities and snapshots, encrypted reasoning, failure/EOF barriers |
| tools | Descriptor-relative paths, traversal/symlinks/FIFOs, UTF-8 line/byte paging with complete-file revisions, atomic writes, schema numeric edges, process environment/FD isolation, cancellation/flood/descendant cleanup |
| parallel | Opt-in concurrency, write/input/approval barriers, cancellation/deadlines, budget enforcement, receipt replay after task budget exhaustion, ordered receipts and worker timing |
| context | Over-budget/failed-turn compaction recovery, retained user constraints/steering, unresolved exchange rejection, transactional forks |
| results | Bounded previews, exact UTF-8 retrieval, session/fork access, stable history, new/legacy pause names |
| recovery | Whole-batch process-crash recovery, no silent retries, version 1 schema migration/backfill |
| cache | Stable per-session keys across reopen/resume/checkpoint; fork and new-session isolation |
| observations | SHA-256 known-answer/boundary behavior, active-content reuse, forced reads, changed revisions, compaction/fork invalidation, no reuse of generic previews |
| workflow | Real dispatch and durable phase evidence, contextual search/pagination, edit-plus-check, fixed checks/cache invalidation, unknown effects |
| changes | Exact bytes after multi-hunk edits, duplicates/overlap, stale bases, UTF-8 at file edges, preflight and checkpoint faults during multi-file writes |
| bench_parallel_receipts (AH_BENCHMARK) | End-to-end ordered contents of four distinct files and four delayed callbacks at concurrency 1 and 4 |
| find | Independent line-mask oracle, seeded Unicode/CR/overlap cases, dense matches, unaligned ASCII/UTF-8 validation boundaries |
| task | Bounded repair, revision/policy validation, budgets across submissions/restarts, killed verification/repair-intent recovery, explicit reconciliation and local-check adapter |
| http | Scripted real HTTP/SSE, rate/transport retries, no retry after output, auth/MIME/redirect rejection, deadline/cancellation, connection reuse |
| cli | Actual executable: network-to-tool-to-model loop, persisted approval and Ask across restarts, dedupe/fork/checkpoint, RPC |
| rpc | Malformed frames, asynchronous cancel, EOF and signals, busy response, no-token metadata commands |
| eval_cases | Offline positive/negative checks of the fixed live task graders |
| eval_endurance | Six-run barrier/abort, external cutoff, token subset/cumulative accounting, fake runner integration without credentials |
| cache_trace | Content-free prefix fingerprints, trace redaction, usage subsets, controlled workload and diagnostics fallback |
| cache_transport (AH_LIVE_EVAL) | Real C transport against loopback: key omission, stable keys, previous-response diagnostics and xhigh |
| host_auth | Fake vault/provider: persisted reuse, rotated refresh, cross-process lock, callback state/PKCE, grant errors; signed JWT checks when optional dependencies are installed |
| embed | A C client with no product dependencies |
| python_entrypoints | Credential-free package CLI startup and execution of a copied grader outside the task workspace |

Run normal and sanitizer builds, then `scripts/check-standalone.sh`. The latter
also checks installed CMake consumption. C unit-test assertions remain enabled in
Release builds. CI is provided for macOS and Linux; a workflow file is not evidence
that a remote CI job has already run.

For coverage-guided parser fuzzing, use upstream Clang with its libFuzzer runtime:

```sh
CC=clang cmake -S . -B build-fuzz -DAH_FUZZ=ON
cmake --build build-fuzz --target fuzz_stream
mkdir -p /tmp/ah-fuzz-corpus
cp tests/corpus/stream/* /tmp/ah-fuzz-corpus/
./build-fuzz/fuzz_stream /tmp/ah-fuzz-corpus -max_total_time=60 -max_len=65536
```

Apple's bundled Clang may not include the libFuzzer runtime; use an upstream LLVM
installation in that case. Fuzzer temporary corpora/artifacts should stay outside
the source tree.

On this macOS host, Homebrew LLVM's fuzzer also required its matching C++ runtime:
`-DCMAKE_EXE_LINKER_FLAGS='-L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++'`.
The system C++ library lacked a symbol used by that LLVM build. With the matched
runtime, the 0.3 stream changes passed 20,000 libFuzzer/UBSan iterations; this
toolchain setting is not a production C dependency.

Before claiming live provider support in a release environment, run a separately
authorized smoke test with its actual model/catalog and OAuth/API grant: text,
tool loop, reasoning continuation, wait/approval, cancellation, and usage limits.
Fixture tests validate client semantics, not a provider account's entitlement.

Benchmark reporting must include model and service route, compiler/options,
dependency versions, machine/OS, workload, repetitions, warm/cold state, correctness,
local overhead, end-to-end latency, retained bytes and RSS. Compare identical tools
and budgets. No industry-best or performance-superiority claim is established by
these tests. External executor cancellation is cooperative and arbitrary shell
side effects cannot be transactionally rolled back.

## Recorded evidence

- [Local runtime/file-tool measurements](evaluations/local-performance.md)
- [Terra endurance comparison](evaluations/endurance-evaluation.md)
- [Cache routing investigation](evaluations/cache-evaluation.md)
- [Sol endurance comparison](evaluations/sol-xhigh-evaluation.md)
- [Workflow profile smoke comparison](evaluations/execution-evaluation.md)

Historical claims are tied to the versions and workloads in those records. Current
optimization questions and the required measurements are in [performance](performance.md).
