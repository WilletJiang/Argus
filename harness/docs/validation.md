# Validation and release gates

All default tests are local and deterministic. They do not contact OpenAI or
consume subscription/API quotas. Tests create temporary state and clean it up.

| Suite | Coverage |
| --- | --- |
| core | Native tool loop, duplicate commands/call IDs, approval/input resume, fork/checkpoint, cancellation/steering, process-crash recovery, receipt-write failure |
| alloc | Per-document allocation budgets, sticky builder failure, serialization/realloc failure, no partial JSON |
| stream | Every fragmentation width, UTF-8/CRLF/BOM, duplicate/oversized/deep JSON, terminal barriers |
| tools | Descriptor-relative paths, traversal/symlinks/FIFOs, UTF-8 line/byte paging, exact edits, atomic writes, schema numeric edges, process environment/FD isolation, cancellation/flood/descendant cleanup |
| parallel | Opt-in concurrency, write/input/approval barriers, cancellation/deadlines, budget enforcement, ordered receipts/model input |
| context | Over-budget/failed-turn compaction recovery, retained user constraints/steering, unresolved exchange rejection, transactional forks |
| results | Bounded previews, exact UTF-8 retrieval, session/fork access, stable history, new/legacy pause names |
| recovery | Whole-batch process-crash recovery, no silent retries, version 1 schema migration/backfill |
| http | Scripted real HTTP/SSE, rate/transport retries, no retry after output, auth/MIME/redirect rejection, deadline/cancellation, connection reuse |
| cli | Actual executable: network-to-tool-to-model loop, persisted approval and Ask across restarts, dedupe/fork/checkpoint, RPC |
| rpc | Malformed frames, asynchronous cancel, EOF and signals, busy response, no-token metadata commands |
| eval_cases | Offline positive/negative checks of the fixed live task graders |
| eval_endurance | Six-run barrier/abort, external cutoff, token subset/cumulative accounting, fake runner integration without credentials |
| embed | A C client with no product dependencies |

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

## Local measurement (2026-11-03)

Apple M2, macOS 26.5.2, Apple Clang 21, Release `-O3`, system libcurl 8.7.1,
SQLite 3.51.0, yyjson 0.12.0. `bench_runtime` performs 100 sequential synthetic
text-only turns, with growing history, no observer, no network and no tool process.
The measured interval includes input submission and execution/receipt persistence.

| Store | P50 | P95 | Maximum |
| --- | ---: | ---: | ---: |
| In-memory SQLite | 0.126 ms | 0.150 ms | 0.254 ms |
| SQLite file, DELETE/FULL | 1.498 ms | 3.144 ms | 3.467 ms |

This text-only baseline was recorded at commit `5a49093`. It is one local sample,
not a model-latency benchmark or a cross-harness ranking.
Run your own sample with `-DAH_BENCHMARK=ON` and `./build/bench_runtime`.

The seeded stream fuzzer completed 50,000 runs with upstream LLVM/libFuzzer + UBSan.
The full suite and a separate mutation driver also ran with Apple ASan/UBSan.
On this macOS version the installed Homebrew ASan runtime stalled during its own
initialization, so it was not used for the coverage-guided run. Apple Clang's
ASan test builds worked. Use compatible compiler/runtime combinations; a sanitizer
startup failure is not a passed test or an application finding.

A separately authorized live subscription run covered text/tool execution on
`gpt-6.1-sol`: both this harness and Codex produced passing outputs on three fixed
synthetic tasks. Initial harness requests exposed an absent Content-Type header,
now handled by strict SSE validation. Codex had one timeout with correct output;
a labeled rerun completed. Generated reports and outputs were removed afterward.
Live Ask, cancellation and other models remain separate validation work.


## File tool measurement (2026-11-03)

`bench_files` invokes the production callbacks directly. This comparison uses the
same 4 KiB/64 KiB fixtures, 512 KiB capture budget, 20 warm-ups and 200 measured
runs with Release `-O3`. Reads return the complete requested fixture, with a warm
OS cache; writes include atomic rename plus file and directory `fsync`. Times
include tool argument validation and JSON construction, but exclude engine/SQLite,
network and model inference. The updated reader uses 1000-line pages here so its
returned byte counts match the old whole-file reader.

| Callback | Prior P50/P95 | Updated P50/P95 | Content bytes |
| --- | ---: | ---: | ---: |
| Read 4 KiB | 30 / 47 µs | 24 / 25 µs | 4096 |
| Read 64 KiB | 104 / 150 µs | 96 / 125 µs | 65536 |
| Atomic write 4 KiB | 177 / 203 µs | 176 / 198 µs | 4096 written |

The prior source is commit `5a49093`. This is one local sample; cache, scheduler,
filesystem and durability semantics affect the result. It does not establish a
universal speedup. Run `./build/bench_files [runs]` after configuring
`-DAH_BENCHMARK=ON`. The benchmark removes its temporary workspace.


## Read batch measurement (2026-11-03)

`bench_parallel --file-store` compares concurrency 1 and 4 with the same current
runtime. The provider is a mock; every run starts a fresh session and completes
one model/tool/model cycle with four read calls. The timed interval includes
submit, scheduling, SQLite and ordered receipts, plus correctness checks in the
mock provider. Result projection is disabled equally in both modes. There are two
warm-ups, 100 measured file workloads and 40 measured synthetic latency workloads.

| Store / workload | Serial P50/P95 | Four workers P50/P95 |
| --- | ---: | ---: |
| In-memory / four reads of one 4 KiB file | 0.731 / 1.340 ms | 0.518 / 0.548 ms |
| DELETE/FULL / same file reads | 8.187 / 11.613 ms | 4.179 / 5.080 ms |
| In-memory / four synthetic 20 ms reads | 107.649 / 118.667 ms | 29.135 / 31.750 ms |
| DELETE/FULL / same synthetic reads | 127.149 / 138.579 ms | 36.782 / 39.201 ms |

The synthetic workload requests a 20 ms sleep per callback; OS scheduling can
overshoot. It is not a remote-filesystem or network measurement. Batching also
reduces durability transactions, so these numbers do not isolate thread speedup.
The benchmark checks exact returned file contents, all four receipts and their
order; it prints raw sample arrays and removes its temporary files. It establishes
neither model-latency improvements nor performance against other harnesses.


The subsequent Terra 6.1 xhigh comparison includes three parallel paired trials,
reported token/cache breakdowns, and a separate result-quality audit. See
[the full evaluation](endurance-evaluation.md).
