# Local tool audit · 2026-11-06

Apple M2, macOS 26.5.2, Apple Clang 21, native Release builds of `36c506e` and `e62b6fd`. Three paired blocks used the same fixtures and drivers, in old/new, new/old, old/new order. Values are the medians of the three block P50s and P95s, respectively. The machine was shared and the filesystem cache was warm. Model inference, network and human response time were outside the measurements.

| Timed operation | Unit | Before P50 / P95 | After P50 / P95 |
| --- | --- | ---: | ---: |
| `read_file`, 4 KiB | µs | 16 / 17 | 14 / 15 |
| `read_file`, 64 KiB | µs | 72 / 75 | 64 / 65 |
| `write_file`, atomic 4 KiB | µs | 147 / 174 | 147 / 186 |
| `apply_changes`, two replacements in 8 bytes | µs | 161 / 186 | 159 / 190 |
| `apply_changes`, 1 replacement in 1 MiB | ms | 2.531 / 4.04 | 2.061 / 3.524 |
| `apply_changes`, 8 replacements in 1 MiB | ms | 5.453 / 6.737 | 2.183 / 2.764 |
| `apply_changes`, 64 replacements in 1 MiB | ms | 29.823 / 30.992 | 3.437 / 4.594 |
| `find`, 1,200 files, three hits | ms | 28.683 / 28.837 | 26.919 / 27.111 |
| `find`, same corpus, zero hits | ms | 28.631 / 28.723 | 26.865 / 26.974 |
| `exec`, /usr/bin/true | ms | 0.901 / 0.961 | 0.901 / 0.924 |
| `exec`, shell closes both output streams before exit | ms | 30.077 / 30.704 | 3.959 / 4.022 |
| `check`, execute a passing preset | ms | 3.123 / 25.42 | 4.36 / 4.489 |
| `check`, verified cache hit | µs | 24 / 24 | 26 / 26 |
| `read_result`, 512 bytes from a 1 MiB receipt | µs | 121 / 125 | 17 / 23 |
| `ask`, pause and immediate reply | µs | 236 / 253 | 236 / 247 |
| `wait`, pause and immediate resume via reply | µs | 231 / 249 | 240 / 257 |
| Pending lookup after 4,097 completed operations | µs | 243 / 246 | 9 / 9 |

## Independent reads

One additional block on the revised build timed submission through the second mock response. Four distinct files contained different known bytes. The provider verified each call ID, the complete contents, and receipt order. The delayed callbacks each requested a 20 ms sleep. The file-backed cases include SQLite DELETE journaling and FULL synchronization. Batching also reduces the number of transactions.

| Store and workload | 1 worker P50 / P95, ms | 4 workers P50 / P95, ms |
| --- | ---: | ---: |
| Memory, four 4 KiB files | 0.779 / 1.151 | 0.626 / 0.663 |
| Memory, four delayed callbacks | 103.807 / 115.685 | 28.603 / 31.423 |
| File-backed, four 4 KiB files | 7.276 / 9.251 | 5.252 / 7.097 |
| File-backed, four delayed callbacks | 122.809 / 135.302 | 36.097 / 39.770 |

## What each fixture exercises

- File callbacks used `bench_files 250` with 20 warmups. Reads include UTF-8 validation, complete-file SHA-256 and receipt encoding. Writes include file and parent-directory synchronization. Separately, 1,100 deterministic old/new receipts matched across byte and line offsets, output limits, binary tails, Unicode and large files. Successful slices and hashes also matched independent Python byte slicing and hashlib.
- For literal search, `bench_find` scanned the 19,660,800-byte ASCII corpus in the [fixture recipe](find-performance.md). Each block used 3 warmups and 21 timed calls per query. Complete result JSON, including snapshot and excerpt revisions, matched across all blocks.
- The large edit fixture repeats an 89-byte C source line to 1 MiB, inserts `unique_%04u` at byte `8192 + 15000*i` for each hunk, then replaces each marker with `replacement`. Each block used 4 warmups and 56 measured calls. Fixture reset and independent file inspection occurred outside the timer. Timed work includes the real atomic replacement and a validating in-process checkpoint callback. The separate workflow suite exercises SQLite-backed checkpoints.
- Command, check, pause and history fixtures used 10 warmups and 120 measured samples per block. The early-close command was `/bin/sh -c "exec 1>&- 2>&-; exit 0"`. The passing preset launched a real helper, read a five-byte dependency and recorded one run. The cache case checked its receipt on every iteration. Short file-edit and process measurements include parsing the tool receipt in the caller.
- Pause measurements include a new in-memory session, two mock responses, a recorded input request and an immediate host reply. History measurements used file-backed SQLite. The page probe checked 512 returned bytes against a 1 MiB JSON string at rotating byte offsets. The queue probe queried a session containing 4,097 finished operations.

## Implementation and boundaries

Reads compute their page and full-file revision in one pass for files up to 1 MiB. Larger files retain direct byte seeking. Bulk edits choose a rare byte only when its frequency bounds the work of checking candidate matches. Every match is verified against the original bytes before conflict and overlap checks. Directory search uses entry types when available and charges bytes on the actual opened descriptor.

The pending index contains unfinished operations. UTF-8 journals serve a page through a read-only SQLite BLOB handle. UTF-16 journals convert stored text to preserve the public UTF-8 offsets. The page encoder checks both the character boundary and the encoded output budget. [SQLite incremental reads](https://www.sqlite.org/c3ref/blob_read.html) · [Database encoding](https://www.sqlite.org/pragma.html#pragma_encoding).

The scheduler batches adjacent declared parallel-safe reads. Mutations, process execution, approvals, input and journal publication retain their ordering. Worker intervals now start once at callback entry. Recovery publishes already-charged receipts after the task budget is exhausted. The budgets and deadlines still apply to new work.

Use `-DAH_BENCHMARK=ON` to build the file, search and parallel drivers. `bench_parallel --file-store` runs both stores. The focused audit probes were temporary. The committed `changes`, `workflow`, `results`, `parallel` and `tools` suites retain the correctness cases. These timings describe local mechanisms. End-to-end task latency with a subscription model requires a separate paired run.
