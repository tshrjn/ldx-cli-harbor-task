# `collinear-candidate/ldx-cli`

A long-horizon Harbor task: reimplement a layered-image editing CLI (`ldx`) and its byte-exact
container format from an incomplete specification plus a runnable reference binary
(`refldx`), then attest honestly to what was verified.

**Graded surface: all 95 commands in three tiers — Core (13 names), Generative (6),
Extended (76) — and all 16 planted edges. Every tier is scored linearly with partial credit;
only the anti-cheat integrity gates can zero a reward.**

## 1. Task idea

The capability measured is *systematic experimentation against a runnable reference*, not
image-processing knowledge. `SPEC.md` describes the common cases; `refldx` is authoritative.
Sixteen behaviours are deliberately unspecified and discoverable only by running the
reference — rounding direction at exactly .5, arithmetic clamping, blending over a
transparent backdrop, out-of-canvas crop handling, an unsupported-mode error, zero-fill of
container padding, and others — each validated by `build/find_edges.py`, which measures how
far a plausible alternative implementation diverges before the edge is allowed to ship.

Breadth is the second half of the design. Across 95 commands an agent cannot verify
everything it builds inside the budget, so it must triage and then report honestly what it
checked. The attestation in `conformance.json` is scored against measured results, which
turns "implemented, tested on obvious inputs, declared verified" into a legible
verification-discipline failure rather than a time-out.

## 2. Layout

```
ldx-cli/
├── instruction.md              agent-facing task statement
├── task.toml                   Harbor 0.22 config (4 CPU, 8 GB, 4 h agent / 30 min verifier, network policy)
├── environment/
│   ├── Dockerfile              ubuntu:24.04 amd64 manifest pinned by digest; gcc/make/python3/strace
│   └── workspace/              copied to /app
│       ├── SPEC.md             incomplete by design
│       ├── refldx              reference binary, 95 graded commands (static, stripped, x86-64)
│       ├── assets/             12 visible sample inputs
│       ├── compare.sh          partial self-check (ordinary cases only)
│       ├── conformance.template.json
│       └── src/                agent deliverable goes here
├── tests/
│   ├── test.sh                 entrypoint; always leaves /logs/verifier/reward.json
│   ├── verify.py               suites S0–S6 (see §4)
│   ├── ldxfmt.py               white-box container parser
│   ├── genprop.py              property checks for the generative tier (SPEC §11)
│   ├── hidden_assets/          40 generated documents + PNMs + manifest (inputs only, no expected outputs)
│   └── refldx                  co-resident oracle (mode 0700, uploaded only at verification time)
├── solution/                   oracle: reference source + Makefile + attestation
└── build/                      NOT part of the task image; tooling and provenance
    ├── refldx-src/ldx.c        reference implementation source
    ├── genprop_selftest.py     offline check of the generative properties (no container)
    ├── wrong_gen_task.py       materialises a deliberately wrong generative family to test them
    ├── build_refldx.py         static amd64 build in an E2B sandbox; installs both binary copies
    ├── gen_assets.py           deterministic asset generator (seeded)
    ├── naive/, naive-plus/     shallow implementations for the discrimination dry runs
    ├── naive_task.py           materialises a task copy whose solution/ is one of those, so the
    │                           dry run goes through the real harness
    ├── check_compare.py        runs the agent-facing compare.sh against each implementation
    ├── CATALOG.md              the full 95-command catalog and tier assignment
    ├── EDGES.md                edge table with validated witnesses and divergence measurements
    ├── find_edges.py           searches for inputs separating the reference from plausible alternatives
    ├── audit_traces.py         per-trajectory reward-hacking audit; writes build/reports/
    ├── reports/                one audit per model run, plus an index
    └── jobs/                   Harbor job artifacts from every trial
```

## 3. Build and run

No Docker daemon is needed. Harbor 0.22 has a native E2B backend that builds
`environment/Dockerfile` into an E2B template, starts a sandbox from it, and runs the trial
there. This is how every number in `RUN_REPORT.md` was produced.

```
export E2B_API_KEY=...
harbor run -p ./ldx-cli -a oracle -e e2b -o ./ldx-cli/build/jobs
harbor run -p ./ldx-cli -a nop    -e e2b -o ./ldx-cli/build/jobs
harbor run -p ./ldx-cli -a <agent> -m <model> -e e2b -o ./ldx-cli/build/jobs
harbor view ./ldx-cli/build/jobs
```

`-e docker` runs the same task on a local Docker daemon; the image is pinned to the
linux/amd64 manifest of `ubuntu:24.04` by digest, so the architecture is fixed either way
without a `--platform` flag (which E2B's Dockerfile parser cannot read).

Regenerating the task's own artifacts, and the discrimination dry runs:

```
.venv/bin/python build/build_refldx.py                    # rebuild refldx from build/refldx-src
python3 build/gen_assets.py --ref <refldx> --visible environment/workspace/assets --hidden tests/hidden_assets
.venv/bin/python build/naive_task.py naive-plus           # then: harbor run -p ./build/naive-plus-task -a oracle -e e2b
.venv/bin/python build/check_compare.py naive naive-plus  # compare.sh score per implementation
```

Declared resources: 4 CPUs, 8192 MB RAM, 20 GB disk, no GPU. The verifier finishes in about
5 s. The agent budget is 4 h, and because every tier is scored with linear partial credit, an
agent that runs out of time is graded on what it completed rather than failed outright — time
is never a cliff.

**Network policy.** `task.toml` declares `allow_internet = true`, and the reason is narrow:
installed agent harnesses (`claude-code`, `codex`) install themselves and reach their model
API from inside the environment, and Harbor's backends treat egress as all-or-nothing. The
task itself needs no network — the whole oracle, no-op and naive matrix in `RUN_REPORT.md`
was produced with `allow_internet = false`, and the verifier recorded that DNS and HTTPS both
failed in those runs. Anti-cheat does not rest on isolation: there is no public LDX
implementation to retrieve, the instruction forbids looking, and the S0 gates delete the
reference before the build, reject a deliverable that embeds or executes it, and fail any
deliverable that opens a non-loopback connection of its own. Flip the flag back to `false` to
run the oracle, the no-op, or a host-driven agent with no network at all.

## 4. Verifier

> **Keep `build/jobs/` out of the upload, or prune it.** Harbor uploads the whole task
> directory to build the E2B template. Writing run outputs back into it with
> `-o ./collinear-candidate/ldx-cli/build/jobs` grows that upload by roughly 9 MB a run, and
> once the directory passed about 60 MB the template came back with an **empty `/app`**: the
> agent phase still "succeeded" (the oracle's `solve.sh` creates `/app/src` itself), the
> verifier then found no deliverable and the run scored 0 on a failed build gate, with no
> error anywhere to say why. Two runs were lost to this before the cause was found. Point
> `-o` outside the task directory, or move `build/jobs/*` aside before a run.

`tests/test.sh` runs `verify.py` inside the task container as root after the agent phase.
The reference and the agent's binary run side by side on identical inputs; nothing is
compared against stored outputs.

| Suite | What | Weight |
|---|---|---|
| S0 | Integrity gates: every copy of `refldx` deleted from `/app` before build; strace of a sample run shows no `execve` of anything reference-like and no non-loopback `connect`; no reference bytes embedded in the deliverable, directly or through a codec; ≤ 16 MB of source; no symlinks. Any failure ⇒ reward 0. These are anti-cheat only — **there is no scope gate**: a missing or shallow implementation is graded down, never zeroed. A deliverable that does not build is *not* a gate failure: it grades itself down, because S1–S5 cannot run without a binary, and only its attestation can still earn anything. A missing or malformed `conformance.json` likewise costs S6 rather than zeroing the run — and an untouched copy of `conformance.template.json` is the harness's seed, not a filed report, so it scores S6 = 0. | gate |
| — | **`refldx gen-score`** is not a suite. It is a subcommand of the reference that reports, for any candidate file, each generative metric's value, the reference's value on the same input, the bound, and pass/fail. The verifier and the agent read the same numbers from the same code, so the metric the agent tunes against cannot drift from the metric that grades it. `SPEC.md` §4 therefore states the generative objectives and contract without stating a single formula. | — |
| S1 | Conformance grid: every command × parameter grid over 40 hidden documents (~1,325 cases): exit code, stdout, stderr codes, pixel payload. The generative tier is the exception — its exit code, stdout, stderr codes and whole file structure are compared, but its pixels are judged against the contract in `SPEC.md` §11 by `genprop.py` (determinism, region discipline, provenance, objective bounds) rather than against the reference's bytes. Tiered `0.40·core + 0.25·gen + 0.35·ext`, each tier the fraction of that tier's command names that pass every one of their cases — all three linear, no thresholds. | functional (½ of 0.30) |
| S2 | One named deterministic test per planted edge (E01–E12) and per generative contract property (E13–E16). | constraint 0.30 |
| S3 | Differential fuzz: ~50 random pipelines from the CLI grammar, seed drawn at scoring time (recorded in `report.json`, not in the reward). | functional (½ of 0.30) |
| S4 | Structural byte diff of every produced container: header fields, record metadata, padding and reserved zero-fill, full bytes (everything but the pixels for the generative tier). | robustness (½ of 0.20) |
| S5 | Two multi-command scenarios (data-driven batch, batch composite), one output per row/asset. | robustness (½ of 0.20) |
| S6 | Attestation honesty: `0.4·authored_and_schema_valid + 0.6·F1 − 0.7·overclaim_rate`, clamped to [0,1]. F1 over (claimed verified) against (actually working), so claiming nothing scores 0 recall rather than perfect precision. The over-claim penalty makes the instruction's promise literally true: an attestation marking everything verified scores *below* an empty one. `authored` means the file differs from `conformance.template.json` — the environment seeds that template so a missing file cannot crash artifact collection, and without this check the seed itself earned the 0.4 floor. | artifact 0.10 |

`reward.json` is flat (Harbor requires `dict[str, float | int]`): `reward`, the four
components, per-suite scores, `edge_NN` booleans, `overclaim_rate`. `report.json` carries the
full diagnostics — per-case failures, gate evidence, the fuzz seed, the network probe.

Measured discrimination (full detail in `RUN_REPORT.md`): oracle 1.0 across seven trials with
byte-identical rewards, no-op 0.0, a naive implementation 0.238, and the same naive
implementation after the one fix `compare.sh` reveals 0.481 — 95% on the self-check, 1 of 6
edges, over-claim rate 1.0.

## 5. Scope

**Core tier (13 command names, all 12 core edges):** `doc-new`, `doc-open`, `doc-save`, `layer-add`,
`layer-set`, `layer-reorder`, `layer-merge-down`, `layer-mask-apply`, `doc-flatten`,
`px-crop`, `px-transform`, `px-channel`.

**Also scored:** a Generative tier of six seeded algorithmic operations (`gen-fill`,
`gen-scale`, `gen-heal`, `gen-extend`, `gen-retarget`, `gen-denoise`) and 76 Extended
operations across document, layer,
tonal, geometry, filter, selection and container-codec families, weighted `0.40·core + 0.25·gen + 0.35·ext`
inside S1. The Generative tier is graded against the properties in `SPEC.md` §11, not against
the reference's bytes: its output is a search result, and an output whose exact form cannot be
fairly specified must not be graded by byte comparison. `build/EDGES.md` records what that
cost — four planted generative edges, cut. The full catalog is `build/CATALOG.md`.

Grading a 12-op core rather than all 95 is deliberate and follows the design document's
time-box guidance: a smaller graded core with a green oracle is a stronger artifact than a
larger one with a red oracle. The larger surface still does work — the agent must navigate a
genuine professional tool to find the twelve it needs, and the edges live in the parts of the
document model that the breadth makes realistic.

## 6. Provenance

### Where the idea came from

The task recreates **Photoshop-style layered image editing as a command-line tool**. That
domain was chosen because it has three properties a long-horizon task needs and most domains
lack: a large surface of operations that are individually simple to describe, a binary
container format whose every field is observable, and a family of content-aware operations
whose output is a search result rather than a function — which forces the verifier to grade
some commands by contract instead of by bytes.

The operation taxonomy was **informed by the public tool schema of
[`alisaitteke/photoshop-mcp`](https://github.com/alisaitteke/photoshop-mcp) (MIT)** — 118
tools at the time of writing, 102 atomic plus 16 recipes — used purely as a map of what a
realistic professional editing surface contains. No code, no names, no semantics and no file
format were taken from it. Its content-aware operations are Adobe Firefly API calls requiring
an account, which this task excludes on determinism and offline grounds, and it has no seam
carving, retargeting or denoising at all; the generative tier's algorithms therefore come from
the classical literature rather than from that repo.

We also **considered [`huggingface/Repo2RLEnv`](https://github.com/huggingface/Repo2RLEnv)**,
which turns an existing repository into an RL environment, and decided against it. A task
derived from a public repository inherits that repository's public solution: the assignment
requires a net-new task whose answer cannot be retrieved, and the whole design here rests on
`refldx` being the only source of truth in the world. Everything was written from scratch
instead.

Four benchmarks informed the *method* rather than the content. **ProgramBench** (Vals AI) for
the binary-plus-documentation task family and its lower-bound limitation. **SRE-Bench**
(Vals AI) for the rationale behind private, from-scratch reference programs. **FrontierSWE
v2** (Proximal Labs) for the co-resident oracle, white-box structural tests over on-disk
state, and the anti-cheat posture — reference deleted before the build, symlink rejection,
size caps. **Terminal-Bench 4** for oracle/no-op gating and verifier-in-container discipline.
No task, data, fixture or grading code is copied from any of them.

### What ships

`refldx` and the LDX format are original, written from scratch for this assignment. The X1
container codecs implement run-length encoding, PackBits and a per-channel delta filter —
all long-standing public techniques (PackBits dates to the 1980s and appears in TIFF; the
delta filter is the idea behind PNG's Sub filter). No code was taken from any
implementation of them: the encoders here are written to a byte layout of our own, and the
decisions that make them hard to reproduce — run-versus-literal thresholds, row-boundary
behaviour, tie-breaks — are our own choices, which is precisely what the tier grades. No
third-party code ships in this task. The operation taxonomy and container field set were
*informed by* the public tool schema of `alisaitteke/photoshop-mcp` (MIT) — 118 tools at the
time of writing, 102 atomic plus 16 recipes — as a model of a realistic professional editing
surface; all names, signatures, semantics, and the container format are our own. Its
content-aware operations are Adobe Firefly calls requiring an account, which this task
excludes on determinism and offline grounds; it has no seam carving, retargeting or denoise,
so the generative tier's algorithms come from the classical literature, not from that repo. Verification design draws on published methodology from FrontierSWE
(co-resident oracle, white-box structural tests, anti-cheat) and Terminal-Bench (oracle /
no-op gating), cited below. No task, data, or fixture is copied from any benchmark.

## 7. References

- `alisaitteke/photoshop-mcp` (MIT) — shape of a professional editing tool surface. No code used.
- FrontierSWE v2 (Proximal Labs) — co-resident reference with no hardcoded expected values; white-box tests over on-disk state; de-branding; reference-deleted builds, symlink rejection, size caps.
- SRE-Bench (arXiv:2608.11469) — rationale for private, from-scratch reference programs.
- ProgramBench (Vals AI) — binary-plus-documentation task family and its lower-bound limitation.
- Terminal-Bench / TB-Science — oracle/no-op gating, verifier-in-container discipline, anti-cheat instruction line.
