# Native find performance — 2026-11-05

Version 0.4 renames native `search` to `find`, removes the directory-listing tool,
and merges single replacements into `apply_changes`. This study measures the local
retrieval callback, not model behavior or end-to-end agent latency. No live model
calls were made. The [0.3 live task comparison](execution-evaluation.md) remains
separate and does not establish a general agent speedup.

## Changes measured

- Find the first hit before deriving context; nonmatching files need no line index.
- Keep only six preceding line starts and the current merged range. Multiple hits
  on one line cannot create new context ranges, so advance to the next line once.
- Allocate the observed source length plus its terminator, rather than 1 MiB for
  every file; read that bounded length and reject short reads/observed changes.
- Validate ASCII a word at a time using unaligned-safe `memcpy`, with the existing
  full UTF-8 path for non-ASCII bytes and partial-codepoint handling.

Full source validation, file revisions, complete scanned-input hashing, result
ordering, context merging, byte limits and pagination remain enabled. Every timed
old/new pair returned an identical complete JSON result, including snapshot and
file hashes; performance was not obtained by dropping metadata or scanning less.
There is no subprocess search backend, persistent content cache or new thread pool.

## Protocol and results

The baseline is commit `331fa71`, runtime 0.3.0. Both variants use Release builds
and the same `bench_find` source, compiler, system libraries, corpus and arguments.
The measured region covers one native callback including JSON encoding. Process
startup, fixture creation, journal commits, models and network are excluded.
Each process performs three warmups. The primary run has three alternating-order
paired blocks of 21 measurements per case; the small case has nine blocks of 101
measurements because exploratory timings were noisy. The primary run did not
overlap an intentionally launched build or test; the computer was not exclusive.

Values below are medians of the per-block medians, in milliseconds:

| Case | Files / source bytes | 0.3 search | 0.4 find | Reduction |
| --- | --- | ---: | ---: | ---: |
| Small, sparse hits | 30 / 30,720 | 0.444 | 0.401 | 9.7% |
| Medium, sparse hits | 300 / 1,228,800 | 5.826 | 4.559 | 21.7% |
| Large, sparse hits | 1,200 / 19,660,800 | 49.625 | 30.869 | 37.8% |
| Large, no hit | 1,200 / 19,660,800 | 48.908 | 30.761 | 37.1% |
| Mostly Chinese text | 300 / 4,617,621 | 13.338 | 10.971 | 17.7% |
| Dense overlapping hits on one line | 1 / 65,536 | 1.475 | 0.065 | 95.6% |

The dense case is a stress test for redundant match processing, not a representative
repository-wide average. Small operations remain sensitive to host noise: an
exploratory batch had median block timings of 0.506 ms before versus 0.638 ms after,
and one large after-block had a 65.987 ms P95. Those observations are retained in
the ledger rather than discarded. This study supports the local mechanism under
these conditions; it does not establish cold-storage behavior or general tail latency.

## Correctness and reproduction

The [ledger](find-performance.json) includes all primary block statistics, binary
and measured-source hashes, result hashes and retained exploratory observations.
Before final timing, 80 seeded old/new output comparisons passed. Current regression
tests independently construct a line mask and merge contiguous context ranges:
48 edge cases plus 160 seeded cases cover Unicode, CR, whitespace, overlap and
trailing/empty lines. Another 363 unaligned UTF-8 boundary checks and a 1 MiB dense
overlap case exercise validation and output continuation. Existing workflow tests
cover stale snapshots, symlinks, binary scan budgets and exact excerpt paging.

Build the optional callback driver:

```sh
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DAH_BENCHMARK=ON
cmake --build build-perf --target bench_find --parallel
./build-perf/bench_find /absolute/fixture \
  '{"path":".","query":"needle","context_lines":2,"max_results":100}' 21
```

For the three ASCII sparse corpora, create zero-based `f0000.txt`… names with 30,
300 or 1,200 files of 1,024, 4,096 or 16,384 bytes each. Each line contains 127 `x`
characters plus LF; in files 5, N/2 and N−1 replace the middle line with `needle `,
120 `y` characters and LF. The no-hit case uses the same large corpus and query
`absent-token`. Each Chinese file contains 32 repetitions of 32 copies of
`春夏秋冬雪` plus LF; files 5, 150 and 299 append `needle` plus LF. The dense file
contains 65,536 `a` bytes and uses query `aaa`. Use the same driver source with the
baseline checkout; its old-name lookup is benchmark compatibility, not a runtime alias.

Generated corpora, temporary baseline checkout and builds are removed after compact
records are retained. Public C structs and schema 3 are unchanged; the new model
tool surface and editing preconditions are documented in [workflows](../workflows.md).
