# Validation and release gates

All default tests are local and deterministic. They do not contact OpenAI or
consume subscription/API quotas. Tests create temporary state and clean it up.

| Suite | Coverage |
| --- | --- |
| core | Native tool loop, duplicate commands/call IDs, approval/input resume, fork/checkpoint, cancellation/steering, process-crash recovery, receipt-write failure |
| alloc | Per-document allocation budgets, sticky builder failure, serialization/realloc failure, no partial JSON |
| stream | Every fragmentation width, UTF-8/CRLF/BOM, duplicate/oversized/deep JSON, terminal barriers |
| tools | Descriptor-relative paths, traversal/symlinks/FIFOs, atomic writes, schema numeric edges, process environment/FD isolation, cancellation/flood/descendant cleanup |
| http | Scripted real HTTP/SSE, rate/transport retries, no retry after output, auth/MIME/redirect rejection, deadline/cancellation, connection reuse |
| cli | Actual executable: network-to-tool-to-model loop, persisted approval and Ask across restarts, dedupe/fork/checkpoint, RPC |
| rpc | Malformed frames, asynchronous cancel, EOF and signals, busy response, no-token metadata commands |
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

Apple silicon, macOS 26.5.2, Apple Clang 21, Release `-O3`, system libcurl 8.7.1,
SQLite 3.51.0, yyjson 0.12.0. `bench_runtime` performs 100 sequential synthetic
text-only turns, with growing history, no observer, no network and no tool process.
The measured interval includes input submission and execution/receipt persistence.

| Store | P50 | P95 | Maximum |
| --- | ---: | ---: | ---: |
| In-memory SQLite | 0.126 ms | 0.150 ms | 0.254 ms |
| SQLite file, DELETE/FULL | 1.498 ms | 3.144 ms | 3.467 ms |

This is one local sample, not a model-latency benchmark or a cross-harness ranking.
Run your own sample with `-DAH_BENCHMARK=ON` and `./build/bench_runtime`.

The seeded stream fuzzer completed 50,000 runs with upstream LLVM/libFuzzer + UBSan.
The full suite and a separate mutation driver also ran with Apple ASan/UBSan.
On this macOS version the installed Homebrew ASan runtime stalled during its own
initialization, so it was not used for the coverage-guided run. Apple Clang's
ASan test builds worked. Use compatible compiler/runtime combinations; a sanitizer
startup failure is not a passed test or an application finding.
