# Terra xhigh long-task evaluation — 2026-11-03

This compares the C harness and Codex app-server 0.164.0 on three concurrent paired copies of the same Workboard release task. Both requested `gpt-6.1-terra`, `xhigh`, through the same ChatGPT-plan public Responses endpoint. A loopback wire probe verified Codex actually sent the requested model and reasoning effort; harness live responses confirmed both fields.

The external observation ceiling was 600 seconds. Neither the prompt nor specification disclosed it. There were no follow-up prompts or automatic network retries. Each run had a fresh workspace; all six started behind a barrier. System prompts, tool interfaces and context machinery still differed. Codex used fallback model metadata; the warning did not remove xhigh. Results therefore describe these configured instances, not every Codex Desktop feature.

## Completion and independently checked quality

Four runs finished naturally before the ceiling. Both third trials failed on HTTP 504 before producing implementations. These service failures are retained, not substituted with better attempts. This workload does **not** establish continuous ten-minute operation.

| Run | Seconds | Outcome | Frozen checks | Additional contract audit | Model-authored tests rerun |
| --- | ---: | --- | ---: | ---: | ---: |
| codex-1 | 403.774 | Completed | 33/33 | 17/17 | 18 |
| harness-1 | 207.097 | Completed | 33/33 | 15/17 | 14 |
| codex-2 | 436.401 | Completed | 33/33 | 15/17 | 15 |
| harness-2 | 241.504 | Completed | 33/33 | 17/17 | 15 |
| codex-3 | 15.681 | HTTP 504 | 0/33 | — | — |
| harness-3 | 17.950 | HTTP 504 | 0/33 | — | — |

The original 33 checks were frozen before execution. After the user requested deeper result-quality analysis, the same 18 additional checks were initially applied to all four completed outputs. One simultaneously requires Conflict for a changed idempotency key and NotFound for an unknown dependency; the specification did not define precedence. It is now reported as an unscored ambiguity, leaving 17 clear-contract checks. The initial raw scores were harness 15/18 and 17/18, Codex 18/18 and 16/18; excluding that ambiguity gives the table above. They follow the original specification but were designed after inspecting outputs: this is a post-run diagnostic audit, not a replacement preregistered score. All four preserved the supplied specification and smoke tests, added release tests, wrote usage/architecture documentation, and passed their own tests when independently rerun. No model-produced code was repaired before either grading stage.

Confirmed defects:

- **harness-1:** accepts an invalid state in an old event snapshot; accepts a current job that disagrees with its last event. Both are durable-data validation gaps.
- **harness-2:** passed all 17 clear-contract checks. Its NotFound result in the unscored precedence case is retained as an observation, not classified as a defect.
- **codex-1:** passed all 17 clear-contract checks, including malformed state, historical state consistency, injected replacement failures and transactional rollback. Its implementation also included snapshot checksums. This is evidence of stronger validation in this sample, not proof of defect-free software.
- **codex-2:** accepts an invalid historical event state; rejects a valid heterogeneous inner join because a colliding field occurs only on an unmatched right-side row.

For the join reproduction, left is `[{"k":1,"right.x":9}]`; right is `[{"id":1,"y":2},{"id":2,"x":3}]`. Joining k=id should retain right.x=9 and add right.id/right.y from the matched row. codex-2 instead raises ValidationError. Historical-data tests modify only an older event to an impossible state; I/O tests inject failure before os.replace and check memory, disk, event counters and cleanup.

The successful harness runs averaged 224.301 seconds; successful Codex runs averaged 420.087 seconds. Harness was faster in this sample. The additional audit found two clear defects in one completed output from each runner, while the other output from each passed all checks. There is no defensible overall quality winner from these four outputs, nor evidence of industry leadership or a causal advantage from C itself. Independent acceptance checks are needed before converting an agent final message into a trustworthy product-level “done”.

## Confirmed token usage

Input includes cached input. Output includes reasoning tokens. Do not add either subset again. Completed-run totals are reported usage; failed/interrupted requests may not return usage, so their rows and aggregate spend below are lower bounds. No monetary or subscription-credit savings are inferred from raw token counts.

| Run | Input | Cached input | Uncached input | Output | Reasoning subset | Complete usage |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| codex-1 | 378,142 | 361,856 | 16,286 | 23,699 | 2,410 | yes |
| harness-1 | 99,181 | 36,480 | 62,701 | 12,884 | 1,281 | yes |
| codex-2 | 411,863 | 399,744 | 12,119 | 21,453 | 1,823 | yes |
| harness-2 | 144,075 | 61,440 | 82,635 | 15,044 | 2,065 | yes |
| codex-3 | 5,324 | 3,968 | 1,356 | 102 | 0 | no; lower bound |
| harness-3 | 1,546 | 0 | 1,546 | 58 | 0 | no; lower bound |

For the two successful runs per runner, harness input totaled 243,256 tokens versus Codex 790,005. However, **uncached** input was 145,336 versus 28,405, respectively. Codex had substantially more cache reuse. Outputs were 27,928 versus 45,152. The requested parallel repetitions can share provider cache behavior; this is not a cold-cache experiment.

## Interrupted pilot and access probes

I interrupted an earlier batch while investigating the missing-model-metadata warning. A subsequent actual-wire test showed xhigh had been sent correctly: this interruption was an evaluation operation error, not a model-configuration failure. The pilot is excluded from paired completion comparisons but its known usage is retained here. One pilot harness run completed, one received a failed/incomplete provider response, and four runs were operator-interrupted.

| Pilot run | Recorded outcome | Input | Cached input | Output | Reasoning subset |
| --- | --- | ---: | ---: | ---: | ---: |
| codex-1 | operator_interrupted | 101,873 | 93,568 | 15,440 | 182 |
| codex-2 | operator_interrupted | 118,628 | 105,984 | 13,396 | 163 |
| codex-3 | operator_interrupted | 118,495 | 106,240 | 13,684 | 207 |
| harness-1 | completed | 114,754 | 30,080 | 12,894 | 1,471 |
| harness-2 | failed | 4,552 | 0 | 113 | 0 |
| harness-3 | operator_interrupted | 118,782 | 43,520 | 15,099 | 1,798 |

| Stage | Confirmed input | Cached input subset | Confirmed output | Input + output |
| --- | ---: | ---: | ---: | ---: |
| Formal trials | 1,040,131 | 863,488 | 73,240 | 1,113,371 |
| Interrupted pilot | 577,084 | 379,392 | 70,626 | 647,710 |
| Four live access probes | 11,700 | 0 | 335 | 12,035 |
| All observed usage (lower bound) | 1,628,915 | 1,242,880 | 144,201 | 1,773,116 |

The local wire probe used fake credentials and a loopback fixture, so it consumed no model tokens. Real credentials remained in evaluation process memory only. Raw generated workspaces, trace files, protocol-schema dumps and temporary builds are removed after this report is recorded; reproducible tasks, runners and both graders remain in evals.

## Reproduction identifiers

- Prompt SHA-256: `beaebfe35323ba1b9f3525f5e7423389e9ca0a6e9ba1a3038ab024bead417d9e`
- Specification SHA-256: `e63c2c104b8a4698b6c78e0a741d638b8d163248ab916ab96cff7e73d3911801`
- Frozen grader SHA-256: `c20a46c7a84d7464d795abebc39d6148edbeea67936c0f6fbee626b7fd73d200`
- Post-run audit SHA-256: `d1cd2564b0a0562b27bd5c53f682a45dbf2140f6d229205d1ca138d020e91bd2`

The same hashes were verified after evaluation. See [evaluation instructions](../evals/README.md), [model reference](https://developers.openai.com/api/docs/models/gpt-6.1-terra), and [plan-authorized app-server setup](https://developers.openai.com/siwc/token-sharing-open-source/codex-app-server).
