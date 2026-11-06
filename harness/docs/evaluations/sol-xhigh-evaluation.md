# GPT-6.1 Sol xhigh long-task comparison — 2026-11-04

The fixed harness used fewer reported tokens, while Codex had a small correctness
advantage on this workload. **None of the six scored runs finished the full release
within 600 seconds.** All were externally interrupted. These are quality and usage
measurements at a common observation boundary, not measured natural completion times.

This repeats the previous Workboard task: identical prompt, specification, initial
files and 33 original checks. Both runners requested `gpt-6.1-sol`, `xhigh`, through
the same ChatGPT-plan public Responses endpoint, with three fresh workspaces per
runner and a six-party start barrier. No model was told the time limit; there were
no follow-up prompts or automatic network retries. Codex app-server was 0.164.0.
The primary fixed cohort uses C harness 0.2.2, commit `6c147be`.

## Result quality at the cutoff

| Run | Seconds | Original checks | Additional checks, corrected fixture | Model-authored tests rerun | Release README |
| --- | ---: | ---: | ---: | --- | --- |
| harness-1 | 600.075 | 33/33 | 17/17 | 31 tests; one error | Original 63-byte stub |
| harness-2 | 600.063 | 33/33 | 16/17 | 35 tests; one error | Original 63-byte stub |
| harness-3 | 600.068 | 33/33 | 16/17 | 39 tests; one error | Original 63-byte stub |
| codex-1 | 600.030 | 33/33 | 17/17 | 23 tests; all pass | Original 63-byte stub |
| codex-2 | 600.033 | 33/33 | 17/17 | 24 tests; one error | Original 63-byte stub |
| codex-3 | 600.018 | 33/33 | 17/17 | 28 tests; one error | Original 63-byte stub |

The two harness defects are the same heterogeneous-join boundary error. Given
left `[{"k":1,"right.x":9}]` and right `[{"id":1,"y":2},{"id":2,"x":3}]`, an inner
join on k=id should return `[{"k":1,"right.x":9,"right.id":1,"right.y":2}]`. An
unmatched right-side row's x field must not create a collision. harness-2 and
harness-3 instead raise ValidationError. The other four implementations return
the correct result. This was independently reproduced without modifying outputs.

All five failures in model-authored tests share a different problem: the test
creates a job with default `available_at=0`, then tries to claim it at a negative
time. The specification requires that claim to return None. All six implementations
correctly do so, and claim successfully at time 0. Thus these are unfinished/broken
regression tests, not five additional demonstrated implementation defects. They
still prevent a clean release test run. The uncompleted README is another missing
release requirement. Test count alone is not a measure of test quality.

All supplied specifications and smoke tests were preserved. Both audit stages left
the generated workspaces unchanged. The implementations were not repaired before
scoring. Both runners continued producing tool activity in the final observed
minute, but tool timestamps do not prove useful work during every intervening second.

### Audit-fixture correction and raw scores

The previous 17-check additional audit was frozen before starting this comparison.
One test assumed internal `jobs` storage was a list, although only the public
export format requires a list. Valid implementations using a dictionary produced
a KeyError in the grader before its intended corruption reached the implementation.

After this appeared in the initial cohort, the fixture was corrected to locate the
same current job in either representation. The contract assertion did not change.
The correction was frozen separately before the primary cohort completed, then
applied uniformly to all twelve workspaces. Original scores are retained:

| Primary run | Original audit | Corrected audit |
| --- | ---: | ---: |
| harness-1 | 16/17 | 17/17 |
| harness-2 | 16/17 | 16/17 |
| harness-3 | 15/17 | 16/17 |
| codex-1 | 16/17 | 17/17 |
| codex-2 | 16/17 | 17/17 |
| codex-3 | 17/17 | 17/17 |

The unchanged Conflict-versus-NotFound precedence ambiguity remains unscored.
The persistence-corruption checks still inspect these implementations' storage
layout; the suite is a bounded contract audit, not exhaustive proof of correctness.

## Reported token usage

**Every primary run is a lower bound:** interruption can omit usage for an in-flight
response. Cached input is included in input; reasoning is included in output.
Neither subset is added again. The percentages below describe returned usage,
not a verified complete bill or subscription-credit saving.

| Run | Input | Cached input | Uncached input | Output | Reasoning subset | Input + output |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| harness-1 | 171,825 | 163,456 | 8,369 | 25,938 | 5,192 | 197,763 |
| harness-2 | 147,258 | 124,800 | 22,458 | 26,897 | 4,949 | 174,155 |
| harness-3 | 149,310 | 121,856 | 27,454 | 24,713 | 4,135 | 174,023 |
| codex-1 | 185,639 | 157,440 | 28,199 | 25,259 | 4,096 | 210,898 |
| codex-2 | 190,775 | 161,024 | 29,751 | 26,080 | 4,902 | 216,855 |
| codex-3 | 180,034 | 155,520 | 24,514 | 25,669 | 4,040 | 205,703 |

| Three-run totals | Harness | Codex |
| --- | ---: | ---: |
| Input | 468,393 | 556,448 |
| Cached input subset | 410,112 | 473,984 |
| Uncached input | 58,281 | 82,464 |
| Output | 77,548 | 77,008 |
| Input + output | 545,941 | 633,456 |
| Weighted cached-input rate | 87.56% | 85.18% |

The harness recorded 13.82% fewer total tokens and 29.33% less uncached input;
output volume was similar. Its successful responses all echoed the stable cache
key, and request fingerprints found no prior-history rewrites or stable-field
changes. This supports the routing fix on actual long tasks. It does not explain
every residual miss or isolate C orchestration from prompt/tool differences.

There is no completion-speed winner: all observations are censored at ten minutes.
There is also no complete-release winner. Codex passed two more independent checks
across the three outputs and had one clean generated test suite, which is a modest
quality advantage in this sample, not evidence of universal superiority.

## Initial cohort and newly exposed streaming defect

The first cohort used the already cache-fixed `105d18a`. Each harness run stopped
after one response, with zero tools and an unchanged implementation stub. The
runtime incorrectly considered that a completed turn. A separate live wire probe
showed why: the full function call arrived in `response.output_item.done`, while
`response.completed.output` was empty. The parser had discarded the completed item.

The fix collects completed output items by index and preserves native payloads,
including encrypted reasoning. It still waits for a successful terminal response
before committing or executing tools. Duplicate/conflicting items, gaps, incomplete
items without a final snapshot, and failed/truncated streams are rejected. The
[Responses streaming reference](https://developers.openai.com/api/reference/resources/responses/streaming-events)
describes the completed-item events used by the corrected collector.

A real captured Sol stream replayed correctly through the fixed C transport.
Nineteen suites passed ASan/UBSan, macOS and Linux CI passed, and the updated parser
completed 20,000 libFuzzer/UBSan runs. The local fuzzer required the matching LLVM
libc++ search path; its initial linker failure is not counted as a successful run.
Both runners' live preflight now must read a fixture with a tool before replying.

The initial Codex runs were allowed to continue to the same 600-second boundary.
They are retained separately, not substituted into the primary comparison:

| Initial run | Seconds | Original / corrected additional checks | Input | Cached | Output |
| --- | ---: | --- | ---: | ---: | ---: |
| harness-1 | 3.884 | 0/33; 0/17 | 743 | 0 | 18 |
| harness-2 | 5.169 | 0/33; 0/17 | 743 | 0 | 18 |
| harness-3 | 4.168 | 0/33; 0/17 | 743 | 0 | 18 |
| codex-1 | 600.028 | 33/33; 17/17 | 208,117 | 191,232 | 24,772 |
| codex-2 | 600.022 | 33/33; 17/17 | 153,495 | 137,600 | 19,946 |
| codex-3 | 600.032 | 33/33; 17/17 | 166,253 | 150,656 | 26,011 |

The initial Codex raw audit scores were 17/17, 16/17 and 16/17 before correcting
that same fixture. Its own tests were 25 with one error, four smoke tests passing,
and 28 with one failure. All initial READMEs also remained the original stub.

## All observed usage, including diagnosis

| Stage | Input | Cached subset | Output | Input + output |
| --- | ---: | ---: | ---: | ---: |
| Initial six-run cohort | 530,094 | 479,488 | 70,783 | 600,877 |
| Initial two access probes | 11,372 | 3,328 | 66 | 11,438 |
| Fixed six-run cohort | 1,024,841 | 884,096 | 154,556 | 1,179,397 |
| Fixed two tool-loop probes | 22,994 | 19,200 | 81 | 23,075 |
| Streaming diagnosis | 57 | 0 | 19 | 76 |
| Total lower bound | 1,589,358 | 1,386,112 | 225,505 | **1,814,863** |

The reasoning subset across all reported output is 40,609 tokens. Local wire probes,
replays, grading and fuzzing made no inference calls. Persistent subscription login
refreshed automatically from Keychain; there was no fresh browser authorization.

## Reproduction and limits

The [structured record](sol-xhigh-evaluation.json) contains all twelve trials,
raw/corrected grading, four access probes, the streaming diagnostic, source and
binary hashes, per-request harness cache measurements, and observed usage totals.
The task hashes match the prior Terra evaluation:

- Prompt: `beaebfe35323ba1b9f3525f5e7423389e9ca0a6e9ba1a3038ab024bead417d9e`
- Specification: `e63c2c104b8a4698b6c78e0a741d638b8d163248ab916ab96cff7e73d3911801`
- Original 33-check grader: `c20a46c7a84d7464d795abebc39d6148edbeea67936c0f6fbee626b7fd73d200`
- Original additional audit: `d1cd2564b0a0562b27bd5c53f682a45dbf2140f6d229205d1ca138d020e91bd2`

Both sessions used the same model request, endpoint and grant, but system prompts,
tool APIs, context sizes and verification behavior differed. Codex requested model
and effort were checked on the wire; harness responses confirmed both. Parallel
trials can share provider cache and service load; this is not a cold-cache study.
Three trials of one task cannot establish broad quality or causal speed advantages.

Generated sources, workspaces, raw traces, probes and temporary builds are removed
after the compact records are saved. Keychain authorization remains available.
See [evaluation instructions](../../evals/README.md) and the
[requested Sol model](https://developers.openai.com/api/docs/models/gpt-6.1-sol).
