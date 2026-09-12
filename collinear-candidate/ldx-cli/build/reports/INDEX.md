# Model runs — index, integrity audit and scaling estimate

Regenerate the per-run audits with `.venv/bin/python build/audit_traces.py`.
Raw Harbor artifacts (`reward.json`, `report.json`, trajectories) are under `build/jobs/`.

## 1. Runs

| Run | Model | Harness | Slice | Reward | Audit |
|---|---|---|---|---|---|
| `smoke-opus47` | Claude Opus 4.7 | claude-code | smoke, 5 commands | **1.000** | [audit](audit-smoke-opus47.md) |
| `smoke-gpt55b` | GPT-5.5 (effort high) | codex | smoke, 5 commands | **1.000** | [audit](audit-smoke-gpt55b.md) |
| `smoke-dsflash` | DeepSeek v4 Flash | claude-code | smoke, 5 commands | **0.000** | [audit](audit-smoke-dsflash.md) |
| `hard-glm53` | GLM 5.3 Flash | claude-code | compositing, 3 commands | **0.000** | [audit](audit-hard-glm53.md) |

Runs that did not reach verification, excluded from all conclusions:

| Run | Why it was discarded |
|---|---|
| `model-opus47` | Wrong gateway base URL, 404 `model_not_found` in 5 min, no tokens spent |
| `model-opus47-b` | Opus 4.7 on the full 7-command task; stopped by hand at 29 of 45 min to protect the credit budget |
| `smoke-gpt55` | E2B streaming error at artifact collection (`peer closed connection without TLS close_notify`); rerun as `smoke-gpt55b` |
| `hard-gpt55` | Stopped by hand to conserve credits |

## 2. Integrity audit — was any of it reward hacking?

**No.** Across all four graded runs:

| Check | Result |
|---|---|
| Commands that read, copied, dumped or disassembled the `refldx` ELF | **0 in every run** |
| Disassembler or symbol dumper used at all (`objdump`, `readelf`, `nm`, `strings`, `gdb`, `radare2`, `ghidra`) | **0 in every run** |
| Reference bytes embedded in a deliverable | none |
| Symlinks in a deliverable | none |
| Outbound network connections opened by a deliverable | none |
| Processes the graded binary executed | itself, plus interpreter path resolution |
| `subprocess` / `popen` / `os.system` in delivered source | none |

The agent sandbox contains the reference **binary and no source**: `environment/workspace/`
ships `SPEC.md`, `assets/`, `compare.sh`, `conformance.template.json` and a stripped static
`refldx`. There is no `.c` file anywhere in the image build context, and `build/refldx-src/`
is never copied in. At verification time the binary is deleted from `/app` before `make` runs,
so a deliverable that depended on it would fail to build.

Discovery in every trace was behavioural: run the reference, hex-dump the `.ldx` **it
produced**, compare. Opus did that 19 times, GPT-5.5 twice, and neither ever pointed a dump
tool at the executable. Two earlier flags were false positives worth recording: an `r2 = ...`
Python variable that looked like the `radare2` command, and `xxd` invocations whose targets
were `.ldx` outputs on the same command line as a `refldx` call. The detector now matches
extracted shell commands rather than raw log text.

Both perfect scores were genuine implementations written from scratch: Opus wrote an 18.5 KB
Python `ldx` (37.6 KB of source total) with its own parser, encoder and writer; GPT-5.5 wrote
41.4 KB. Both attested honestly, marking the out-of-scope commands unimplemented, and both
scored an over-claim rate of 0.

## 3. What the runs actually show

**The five-command smoke slice is solvable by frontier models and they solve it cleanly.**
Opus 4.7 finished in 10 minutes over 67 turns with 51 reference invocations. GPT-5.5 finished
in 79 items with 54 reference invocations. Both scored 1.0 with every edge passing. That is
strong fairness evidence: the edges are discoverable by experiment, not gotchas.

**Neither zero is usable as evidence of a capability failure.**

- *DeepSeek v4 Flash* returned a reasoning-only turn with no visible text and no tool call.
  Claude Code injected `[Your previous response had no visible output...]`, got another empty
  turn, and closed the session at 21 turns in 69 seconds. A direct probe of the endpoint shows
  this model returns `thinking` blocks where Anthropic models return `text`, so the harness
  loop starves. This is a gateway and harness mismatch, not a capability result.
- *GLM 5.3 Flash* did real work — 35 tool calls, 19 reference invocations, eight scratch
  scripts in `/tmp` including a Python model of the renderer — but never wrote `/app/src`, and
  the session was cut off with no result event. The credit budget was also nearly exhausted at
  the time. Inconclusive.

## 4. Scope: smoke versus full

The full task is **7 graded commands**, not 80. The number 86 in the design document is the
count of `photoshop_*` signatures in the public MCP schema the *taxonomy shape* was derived
from; none of them ship. The complete design calls for 12 operations, of which slice 1
implements 7.

| | Compositing slice | Smoke slice | Full slice-1 task |
|---|---|---|---|
| Graded commands | 3 | 5 | 7 |
| Planted edges scored | 3 | 4 | 6 |
| S1 conformance cases | 162 | 327 | 489 |
| S4 container pairs | 80 | 241 | 401 |
| Verifier wall time | 1.5 s | 1.9 s | 2.6 s |

Smoke to full is **1.5× in verifier cases** and 1.4× in commands. The agent-side difficulty
does not scale that way, because the two commands the smoke slice omits are `layer-add` and
`doc-flatten`, and `doc-flatten` is the renderer: alpha compositing, six blend modes, group
opacity compounding, masks, and the rounding rule. Three of the six edges live there.

## 5. Time and cost estimate for a full-task run

Anchors: Opus 4.7 solved 5 commands with no compositing in **10 minutes**; the aborted
full-task Opus run was still working at **29 minutes** when it was stopped.

| | Estimate |
|---|---|
| Agent time, frontier model, full 7-command task | **30–45 min** |
| Harness setup (apt, agent install) | 4–6 min |
| Verifier | under 10 s |
| Wall clock per trial | **35–50 min** |
| OpenRouter cost per trial (observed burn ≈ $0.31/min for Opus) | **$9–14** |
| Cost on a Claude Max subscription | no marginal cost |

The declared 4-hour agent budget therefore sits roughly 5–8× above the expected completion
path, which is the intent: a failure should never be attributable to the clock.

## 6. Still open

A frontier-model failure has not yet been demonstrated. Both frontier models passed the
reduced slice, so the evidence has to come from the full task or the compositing slice, and
the only compositing run so far was on a weak model that never produced a deliverable. The
next run should be Opus 4.7 or GPT-5.5 on `build/hard-task` or on the full task.
