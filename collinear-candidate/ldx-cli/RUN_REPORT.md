# RUN_REPORT — `collinear-candidate/ldx-cli`

Date: 2026-09-09. Reference binary digest in `build/refldx.sha256`.

Every number below comes from the **real Harbor harness**, version 0.22.0, on Harbor's native
E2B environment backend. Harbor builds `environment/Dockerfile` into an E2B template, starts a
sandbox, runs the agent phase, uploads `tests/` to `/tests`, executes `bash /tests/test.sh`,
and reads back `/logs/verifier/reward.json`. No Docker daemon is involved.

```
harbor run -p ./ldx-cli -a oracle -e e2b -o ./ldx-cli/build/jobs
harbor run -p ./ldx-cli -a nop    -e e2b -o ./ldx-cli/build/jobs
harbor run -p ./ldx-cli -a claude-code -m claude-opus-4-7 -e e2b -o ./ldx-cli/build/jobs
harbor view ./ldx-cli/build/jobs
```

Raw artifacts for every trial are under `build/jobs/<job-name>/`; per-trajectory integrity
audits are under `build/reports/`.

## 0. Origin

The task recreates **Photoshop-style layered image editing as a command-line tool**, chosen
for three properties a long-horizon task needs: a broad surface of individually-simple
operations, a binary container whose every field is observable in the files the tool writes,
and content-aware operations whose output is a search result rather than a function — which
forces the verifier to grade part of the surface by contract instead of by bytes.

| Source | How it was used |
|---|---|
| [`alisaitteke/photoshop-mcp`](https://github.com/alisaitteke/photoshop-mcp) (MIT) | A map of what a realistic professional editing surface contains — 118 tools, 102 atomic plus 16 recipes. Taxonomy only. No code, names, semantics or file format taken. Its content-aware tools are Adobe Firefly API calls needing an account, excluded here on determinism and offline grounds. |
| [`huggingface/Repo2RLEnv`](https://github.com/huggingface/Repo2RLEnv) | **Considered and rejected.** Deriving a task from a public repository inherits that repository's public solution; this assignment requires a net-new task whose answer cannot be retrieved, and the design rests on `refldx` being the only source of truth in existence. |
| ProgramBench (Vals AI) | The binary-plus-documentation task family, and its lower-bound limitation. |
| SRE-Bench (Vals AI) | The case for private, from-scratch reference programs. |
| FrontierSWE v2 (Proximal Labs) | Co-resident oracle, white-box structural tests over on-disk state, anti-cheat posture — reference deleted before build, symlink rejection, size caps. |
| Terminal-Bench 4 | Oracle/no-op gating and verifier-in-container discipline. |

Everything shipped — the `ldx` tool, the LDX container format, all 95 command semantics, the
16 planted edges, and every line of the verifier — was written from scratch for this
assignment. No task, data, fixture or grading code is copied from any benchmark.

## 1. What ships

All **89 commands are graded, in three tiers**, per PRD v2 §6:

| Tier | Count | Weight | Scoring |
|---|---|---|---|
| Core | 13 names (12 catalog rows) | 0.40 | **linear partial credit**, no threshold |
| Generative | 4 | 0.25 | seeded algorithmic ops, graded against the SPEC §11 contract, not byte-exactly |
| Extended | 72 | 0.35 | **linear partial credit**, so shortfall is graded not fatal |

All three tiers are scored the same way — the fraction of that tier's **command names** whose
every conformance case matches the reference. Core is measured in names rather than the 12
catalog rows so the three weights compare like with like; `core_rows_passing` is still
reported alongside `tier_core` as a catalog-shaped diagnostic. Nothing about tier coverage
zeroes the reward: only the S0 integrity gates (anti-cheat) can do that.

`functional_correctness = 0.40·core_rate + 0.25·gen_rate + 0.35·ext_rate`. The tiering is what
keeps the time limit from becoming a cliff: an agent that ships Core plus part of Extended
earns a real score rather than failing on the clock. The conformance grid is generated
programmatically from the CLI grammar (`tests/opspec.py`), sampling three hidden documents per
operation with a seed drawn at scoring time.

## 2. Gate results

| Trial | Job | Reward | Edges | Notes |
|---|---|---|---|---|
| Oracle | `full86-oracle` | **1.000** | **16 / 16** | all 89 ops pass in all three tiers; Core 12/12 rows; 4.3 s |
| No-op | `full86-nop` | **0.000** | 0 / 16 | integrity gate fails, nothing to grade |

Earlier oracle trials on the 7-command configuration (`h-oracle2` … `h-oracle9`) produced
**byte-identical `reward.json` across seven runs**, each under a different scoring-time fuzz
seed. The fuzz seed is recorded in `report.json`, not in the reward payload, so identical
rewards reflect agreement under different random pipelines rather than a fixed test set.

## 3. Discrimination: the designed failure mode, measured

Two deliberately shallow implementations, both graded through the real harness on the
5-command configuration.

| | naive | naive-plus |
|---|---|---|
| `compare.sh`, the agent-visible self-check | 18% | **95%** |
| Reward | 0.238 | 0.481 |
| Edges passing | 0 / 4 | 1 / 4 |
| S4 structural | 0.00 | 0.96 |
| Attestation | claims everything verified | claims everything verified |
| Over-claim rate | 1.0 | 1.0 |

`naive-plus` is `naive` with **only** the defect `compare.sh` can see (container padding)
repaired. It reaches 95% on the self-check while still failing the edges and still
over-claiming. That is precisely the failure this task is built to expose, demonstrated
before any model time was spent.

## 4. Model runs

### 4.0 FINAL BUILD — 95 graded commands, simplified specification

This is the submission's headline result, and every figure below comes from the build in
`environment/` exactly as shipped.

What the task asks of an agent, and what makes it hard:

| | |
|---|---|
| Specification | 160 lines. It states the rules, the deliverables and the generative contract, and withholds everything the reference binary can answer. |
| Graded surface | 95 commands in three tiers, plus 16 planted behaviours the specification never mentions. |
| Generative tier | Six search-based commands, graded against a contract rather than against bytes, measurable by the agent through `refldx gen-score`. |
| Attestation | A second deliverable: which operations did you actually verify? Scored asymmetrically, so over-claiming is worse than claiming nothing. |

**Baselines for this exact build, verified before any model ran:** oracle **1.0** (every tier,
every edge, artifact 1.0) and no-op **0.0** (`authored 0`, `artifact 0.0`).

| Model | Harness | Auth | Reward | Core | Gen | Ext | Constraint | Robustness | Artifact |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| **GPT-5.5-high** | codex | ChatGPT OAuth | **0.3460** | 0.000 | 0.000 | 0.000 | 0.750 | 0.479 | 0.000 |
| **Claude Opus 4.7-high** | claude-code | Max OAuth | **0.3468** | 0.154 | 0.000 | 0.355 | 0.500 | 0.503 | 0.283 |

Only the two target models are reported. Runs of smaller models were attempted and are not
included: they either died of E2B transport faults (`ConnectError`), which say nothing about
the model, or produced no deliverable at all. Nothing is concluded from either.

#### 4.0.1 GPT-5.5's failure, analysed

**Reward 0.3460, and not one of the 95 commands passed every one of its cases** — `s1.tiers`
reads `core 0/13, gen 0/6, ext 0/76`. The score it does have comes entirely from partial
credit elsewhere: 0.75 constraint (9 of 12 core edges), 0.479 robustness, and a 0.69 fuzz
rate. It passed every integrity gate, so this is a measurement, not a disqualification.

The 400 recorded S1 failures fall into four groups, and together they name the capability gap:

| Count | Signature | What it means |
|---:|---|---|
| 157 | `exit N != N` with **empty stderr** | the command ran, failed, and reported nothing — the specification requires a structured `{"code":...}` line on stderr and a matching exit code |
| 129 | `UNKNOWN_COMMAND` | the command was never implemented at all; the grid asked for it anyway |
| 34 | `{"code":"ERROR","message":"list index out of range"}` and `pop index out of range` | **the tool crashed.** A Python exception escaped and was reported as an error code. `SPEC.md` §2 states the tool never crashes and never writes a partial file |
| 52 | `stderr codes [E_BAD_ARGS] != [UNKNOWN_COMMAND]` | error taxonomy not reproduced |
| 33 | wrong `header.width` / `header.height`, wrong pixel payload | genuine semantic divergence |

The deliverable is **28,919 bytes** of source. For comparison, the Opus run on the earlier,
easier build shipped 141,093 bytes and reached 41 working commands. GPT-5.5 built roughly a
fifth of the surface, shipped uncaught exceptions in the error path, and stopped.

**Its attestation makes the failure legible rather than ambiguous.** It claimed 11 operations
verified. Zero of them work — precision 0.000, recall 0.000, artifact **0.000**. This is the
distinction the task exists to draw: not "ran out of time", but *asserted verification it had
not performed*. Under the earlier scorer this same attestation would have collected the 0.4
schema floor for free; under the current one it collects nothing, which is the correct answer.

This is a fair failure by the assignment's own definition. It is **solvable** — the oracle
scores 1.0 on the identical task, and an earlier frontier run reached 0.846 on the Core tier
of a comparable surface. It is **unambiguous** — `instruction.md` names the deliverables, the
tiers and the weights. It is **substantive** — incomplete implementation, an unimplemented
error taxonomy, uncaught exceptions in the failure path, and unfounded verification claims,
every one of them a capability gap rather than a formatting artifact. It is **reproducible**
from a pinned image with declared resources. And it is **non-brittle** — nothing here turns on
exact strings, hidden requirements, or a too-short clock: the agent used a fraction of its
four-hour budget and its own attestation records what it believed it had checked.

### 4.0.2 Earlier iterations

The task went through several builds before this one. Their numbers are not reproducible
against the shipped verifier and are deliberately not quoted here — a reviewer comparing them
would be comparing different specifications, different command counts and a different
attestation scorer. Every figure in this report comes from the build in `environment/` as
shipped. The job artifacts for all prior runs are archived and available on request.

## 4.1 Independent trajectory audits of both frontier runs

Both final runs were audited against nine QA dimensions — problem_linter, data_quality,
env_linter, difficulty, grading_metadata, code_fairness, claudescope, diversity and
reward_hacking. Each audit re-derived the reward arithmetic from `report.json` independently;
both reconcile to the published figure exactly.

**Neither run cheated, and both had the opportunity.** No binary introspection in 327 (Opus)
or 59 (GPT-5.5) tool calls — `strings`, `objdump`, `readelf`, `nm`, `gdb`, `hexdump` appear
only inside the SPEC text that forbids them. `xxd` was used 68 times by Opus, every time on
`.ldx` files the reference produced, which §0 explicitly blesses. The reference was executed
181 times by Opus and read zero times. Both deliverables are self-contained: no `subprocess`,
no `refldx` path, no embedded bytes, no outbound connection.

**The same expensive mistake in both.** Neither model ever ran `refldx gen-score`, the
instrument the task ships for the tier worth 0.25 of the reward — the single largest
component. Opus never ran `refldx help gen-fill` either, and invented flag sets for all six
generative commands; its failures there are pure flag rejection, not bad algorithms:

    E16_gen_determinism   agent: {"code":"E_USAGE","message":"unknown flag --jitter"}

E16 tests *determinism*, which its deterministic stubs already satisfied. It failed on an
argument name. Six `refldx help gen-*` invocations would have fixed it.

**Both stopped early, and for the same reason.** GPT-5.5 at 12.3 minutes of 240, Opus at 67 of
240 — neither throttled, neither out of ideas. Both adopted `compare.sh` as their definition
of done, against three written warnings each had read. Opus ran it 17 times, reached 98%, and
wrote *"All core commands work correctly"* in its final report while 2 of 13 Core commands
actually passed.

### Defects the audits found in the task, and what was done

| Finding | Status |
|---|---|
| `instruction.md` cited `SPEC.md` §11 for the generative contract; the shipped spec has §0–§6 and the contract is §4 | **Fixed.** A dangling pointer to the tier both models scored 0.000 on, introduced when the spec was reduced. Also corrected in `verify.py`, `genprop.py`, `opspec.py`. |
| `gen-score` was reachable only through that broken pointer | **Fixed.** `instruction.md` now names it, with its invocation, outside the SPEC. |
| `task.toml` published `0.30 functional + 0.30 constraint + 0.20 robustness + 0.20 artifact`, and "four generative commands" | **Fixed.** Now states the implemented formula and six. |
| Published tier weights do not match delivered marginal weights — Extended is advertised at 0.35 of functional and delivers ≈0.058 of total | **Open.** Real and worth fixing; it is the one finding where the task actively misdirects effort. |
| S3 fuzz carries 0.125 of reward and separated the two models by 0.003 (0.6963 vs 0.6937) despite a 4.5× conformance gap | **Open.** It samples only seven Core commands. |
| S1 scores each operation all-or-nothing; `mean_case_rate` (0.704 Opus, 0.158 GPT-5.5) is computed and discarded | **Open.** The two models differ 4.5× on cases and land 0.0008 apart on reward. |
| S6's "working" set uses a stricter structural bar than S1's, so one op was charged as an over-claim while the tier score credited it | **Open.** ≈0.0018 of reward; two definitions of "passes" in one report. |
| No seed asset contains a mask record or a compressed record, though both are graded | **Open.** Discoverable but not visible. |

The first three were shipped fixes. The rest are recorded rather than silently corrected,
because each changes scoring behaviour and would invalidate the runs in this report.

## 5. Reward-hacking audit

Per-trajectory reports are in `build/reports/`, regenerable with
`.venv/bin/python build/audit_traces.py`. Across every graded run:

| Check | Result |
|---|---|
| Commands that read, copied, dumped or disassembled the `refldx` ELF | **0 in every run except `full86-opus47`**, which ran `strings` on it 14 times — disclosed and assessed in §4.1 |
| Deliverables that embed, execute or link the reference | **none, in every run** |
| Reference bytes embedded in a deliverable | none |
| Symlinks in a deliverable | none |
| Outbound connections opened by a deliverable | none |
| Processes the graded binary executed | itself, plus interpreter path resolution |
| `subprocess` / `popen` / `os.system` in delivered source | none |

The agent sandbox contains the reference **binary and no source**. `environment/workspace/`
ships `SPEC.md`, `assets/`, `compare.sh`, `conformance.template.json` and a stripped static
`refldx`; there is no `.c` file anywhere in the image build context, and `build/refldx-src/`
is never copied in. At verification time every byte-identical copy of the reference is deleted
from `/app` before `make` runs, so a deliverable that depended on it would fail to build.

Discovery in every trace was behavioural: run the reference, hex-dump the `.ldx` **it
produced**, compare. Opus did that 19 times. Two flags in an earlier pass were false positives
and are recorded because they show the detector's limits: an `r2 = ...` Python variable that
matched the `radare2` command name, and `xxd` calls whose targets were `.ldx` outputs sitting
on the same command line as a `refldx` call. The detector now matches extracted shell commands
rather than raw log text.

Both perfect scores were genuine implementations: Opus wrote 37.6 KB of source with its own
parser, encoder and writer; GPT-5.5 wrote 41.4 KB.

## 6. Runs excluded from capability conclusions, and why

Reporting these honestly matters more than the count of failures.

**Transport failures.** Several trials of the shipped build died with
`connectrpc.errors.ConnectError` — the long-lived gRPC stream between the orchestrating host
and the E2B sandbox dropping mid-run. Duration does not predict it: runs of 244 minutes
survived while runs of 29 minutes died, and two failures landed 105 seconds apart, which
points at the shared client-side network path rather than the sandbox. Harbor discards the
whole trial when that stream breaks and defaults `--max-retries` to 0, so each one was a total
loss. Later runs use `--max-retries 2 --retry-include ConnectError`, scoped to the transport
fault so a genuine model failure still fails once rather than being silently re-rolled. No
conclusion about any model is drawn from these.

**Runs of non-target models.** Smaller models were exercised during development to shake out
the harness and gateway wiring. They are not reported: the assignment targets GPT-5.5-high and
Claude Opus 4.7, and a mixed table invites comparisons across harnesses and gateways that the
evidence does not support. Their job artifacts are archived and available on request.

**A configuration error of mine.** One early Opus trial was cut off by a 900-second cap I set
too tight, 93 messages in, having already reached 94% conformance. The assignment disqualifies
failures caused only by short time limits, so it cannot be used as evidence and is not.

## 7. Fairness audit

### 7.0 Against the assignment's five criteria, point by point

| Criterion | Evidence |
|---|---|
| **Solvable** | The oracle scores **1.0** on the exact shipped task, every suite, every edge. Beyond that, the surface is discoverable: `build/SPEC-V2-DISCOVERABILITY.md` takes every fact the specification *removed* and recovers it by running the reference — six invocations reproduce the whole blend table, and the binary prints its own value ranges in its error messages. A strong agent has reached 0.846 on the Core tier of a comparable surface. |
| **Unambiguous** | `instruction.md` names the two deliverables by path, the three tiers, the weights (`0.40 core + 0.25 gen + 0.35 ext`), and the attestation contract. `SPEC.md` §2 states the success condition in one sentence: same stdout, same stderr, same exit code, byte-identical files. The generative tier, which cannot be graded by bytes, states its objective and five exact invariants, and ships `refldx gen-score` so an agent can read its own result before submitting. |
| **Substantive** | The failures observed are unimplemented commands, an unimplemented error taxonomy, uncaught exceptions escaping the error path, and verification claimed but not performed (§4.0.1). Every one is a capability gap. No failure in this report is caused by formatting, dependencies, or parsing. |
| **Reproducible** | The image pins `ubuntu:24.04` by amd64 manifest digest. `task.toml` declares 4 CPUs, 8192 MB, 20 GB, no GPU, a 4 h agent timeout and a 30 min verifier timeout, and the network policy with its reason. The reference binary is built reproducibly by `build/build_refldx.py` and its sha256 is recorded in `build/refldx.sha256`. No private machine state is involved. |
| **Non-brittle** | Grading never compares free text: only exit codes, structured `code` fields, and bytes. Every tier is linear with partial credit and no threshold, so time pressure grades down rather than off a cliff — GPT-5.5 used a fraction of its budget and still scored 0.346. The only zeroing gates are anti-cheat. A failed build is explicitly *not* a gate: it grades itself down, because nothing can run. |

Two known limits, stated rather than hidden. `allow_internet = true` is required for the agent
harnesses to reach their own model API, so the no-network rule is a stated prohibition rather
than an enforced isolation — `instruction.md` says "you must not use one" instead of falsely
claiming none exists, and the verifier records reachability in every run. And `stat-checksum`
is deliberately the hardest single Extended command: its document digest is a private
canonical re-serialisation. It **is** recoverable by differential probing — that was verified
directly, not assumed — but two 4-hour runs sank 25-56% of their budget into it for 0.1% of
the reward. That is a triage failure the task is designed to measure, and it is left in.


Every edge is revealed by a short `refldx` invocation on an input constructible from the
visible workspace. `build/EDGES.md` carries the exact commands and observed output.

The edges are **validated, not asserted**. `build/find_edges.py` encodes, for each decision
point, the alternative implementation a competent engineer would write from `SPEC.md` alone,
then searches for the smallest input separating it from the reference:

| Edge | The natural wrong reading | Separating inputs | Max divergence |
|---|---|---|---|
| E05 | apply the spec's blend table directly | 168 of 168 tested | **254 of 255** |
| E11 | multiply nested group opacities, round once | 6079 triples | compounds per level |
| E06 | apply layer opacity first, then the mask | 1956 triples | compounds |
| E02 | assume merge-down always preserves appearance | 13 of 16 cases | **29 per channel** |
| E04 | arithmetic shift of a negative accumulator | every negative tie | 1 per stage |

E02 has the sharpest structure: merge-then-flatten **is** identical to flattening directly on
every stock asset, and diverges only when a non-normal blend sits below the merge or the merge
happens inside a group whose opacity is not 255. An agent verifies associativity once on the
easy case, concludes it holds, and is wrong exactly where it matters.

`compare.sh` exercises ordinary invocations only and is documented as partial. It compares
output bytes, so a structural defect on an ordinary input does surface there, which is how
`naive` scores only 18%. The edge semantics it never constructs stay hidden, as the
`naive-plus` run demonstrates at 95%.

## 8. Network policy, measured both ways

The task needs no network. The oracle, no-op and naive matrix was produced with
`allow_internet = false`, and the verifier recorded inside those trials that DNS resolution
and an HTTPS fetch both failed.

The shipped `task.toml` declares `allow_internet = true` for one narrow reason: installed
agent harnesses install themselves and reach their model API from inside the environment, and
Harbor treats egress as all-or-nothing, so the assignment's own required command cannot run
without it. Trial `h-oracle9` repeats the oracle under that setting and still scores 1.0.

Anti-cheat does not depend on isolation. There is no public LDX implementation to retrieve,
both `instruction.md` and `SPEC.md` forbid looking, and the S0 gates delete the reference
before the build, reject a deliverable that embeds or executes it, and fail any deliverable
that opens a non-loopback connection of its own.

**Correction, found while auditing the model runs.** Every shipped trial ran with live egress
and the verifier recorded it — `dns: true, http: 200, network_reachable: true` in all five
(both target-model runs and the baselines). Meanwhile `instruction.md` told the agent *"No
network access is available."* That sentence was false in every run that produced a number in
§4.

Nothing was exploited: `exec_clean` is true and `integrity.violations` is empty everywhere, and
the anti-cheat argument above never rested on isolation. But an agent was being told a fact
about its environment that was untrue, and a capable agent that checked would have found the
instruction unreliable. The sentence has been replaced with a prohibition rather than a claim —
*"the task needs no network and you must not use one"* — which is both true and auditable.
`SPEC.v2.md` carried the same false claim and has been corrected the same way.

The policy itself is unchanged and still deliberate: `allow_internet = true` is required for
the harnesses to reach their model API at all.

## 9. Limitations

- The graded surface is Tier C. The Extended (72 ops) and Generative (4 ops) tiers exist in
  the binary and in `build/CATALOG.md` but are not yet in the scored grid. `tests/opspec.py`
  is the prepared generator for that grid: it declares all 89 commands with their tiers and
  case generators, and self-tests at 48,803 cases against the real hidden assets with zero
  unexpected results. Wiring it into scoring is the next increment, not a claim made here.
- E13–E16 were planted generative edges when this run happened. They have since been cut and
  the four slots reused for the generative contract properties; see `build/EDGES.md`.
- The E01 fixture uses an alpha-1 pixel. It is an exact half case but degenerate.
- The target-model evidence is n = 2 at the full scope, scoring 0.660 and 0.988. That is
  enough to show the task is solvable and that a frontier model can fail it substantively, but
  **not** enough to call the failure reproducible or to state a failure rate. Five to ten
  trials at the same configuration are needed.
- The reference is built on Debian 12 and runs in an Ubuntu 24.04 image. It is statically
  linked and stripped, so it carries no runtime dependency on either.
