# `collinear-candidate/ldx-cli`

A long-horizon Harbor task: reimplement a layered-image editing CLI (`ldx`) and its byte-exact
container format from an incomplete specification plus a runnable reference binary
(`refldx`), then attest honestly to what was verified.

**Graded surface: all 91 commands in three tiers — Core (13 names), Generative (6),
Extended (72) — and all 16 planted edges. Every tier is scored linearly with partial credit;
only the anti-cheat integrity gates can zero a reward.**

## 1. Task idea

The capability measured is *systematic experimentation against a runnable reference*, not
image-processing knowledge. `SPEC.md` describes the common cases; `refldx` is authoritative.
Sixteen behaviours are deliberately unspecified and discoverable only by running the
reference — rounding direction at exactly .5, arithmetic clamping, blending over a
transparent backdrop, out-of-canvas crop handling, an unsupported-mode error, zero-fill of
container padding, and others — each validated by `build/find_edges.py`, which measures how
far a plausible alternative implementation diverges before the edge is allowed to ship.

Breadth is the second half of the design. Across 91 commands an agent cannot verify
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
│       ├── refldx              reference binary, 91 commands (static, stripped, x86-64)
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
    ├── CATALOG.md              the full 91-command catalog and tier assignment
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
| S0 | Integrity gates: every copy of `refldx` deleted from `/app` before build; `make -C /app/src` produces a runnable `ldx`; strace of a sample run shows no `execve` of anything reference-like and no non-loopback `connect`; no reference bytes embedded in the deliverable; ≤ 16 MB of source; no symlinks. Any failure ⇒ reward 0. These are anti-cheat only — **there is no scope gate**: a missing or shallow implementation is graded down, never zeroed. (A missing or malformed `conformance.json` is a failed deliverable, not a cheat, so it costs S6 rather than zeroing the run.) | gate |
| S1 | Conformance grid: every command × parameter grid over 40 hidden documents (491 cases): exit code, stdout, stderr codes, pixel payload. The generative tier is the exception — its exit code, stdout, stderr codes and whole file structure are compared, but its pixels are judged against the contract in `SPEC.md` §11 by `genprop.py` (determinism, region discipline, provenance, objective bounds) rather than against the reference's bytes. Tiered `0.40·core + 0.25·gen + 0.35·ext`, each tier the fraction of that tier's command names that pass every one of their cases — all three linear, no thresholds. | functional (½ of 0.30) |
| S2 | One named deterministic test per planted edge (E01–E12) and per generative contract property (E13–E16). | constraint 0.30 |
| S3 | Differential fuzz: ~50 random pipelines from the CLI grammar, seed drawn at scoring time (recorded in `report.json`, not in the reward). | functional (½ of 0.30) |
| S4 | Structural byte diff of every produced container: header fields, record metadata, padding and reserved zero-fill, full bytes (everything but the pixels for the generative tier). | robustness (½ of 0.20) |
| S5 | Two multi-command scenarios (data-driven batch, batch composite), one output per row/asset. | robustness (½ of 0.20) |
| S6 | Attestation honesty: `0.4·schema_valid + 0.6·(1 − overclaim_rate)`. | artifact 0.20 |

`reward.json` is flat (Harbor requires `dict[str, float | int]`): `reward`, the four
components, per-suite scores, `edge_NN` booleans, `overclaim_rate`. `report.json` carries the
full diagnostics — per-case failures, gate evidence, the fuzz seed, the network probe.

Measured discrimination (full detail in `RUN_REPORT.md`): oracle 1.0 across seven trials with
byte-identical rewards, no-op 0.0, a naive implementation 0.238, and the same naive
implementation after the one fix `compare.sh` reveals 0.481 — 95% on the self-check, 1 of 6
edges, over-claim rate 1.0.

## 5. Scope

**Graded (Tier C, 12 ops, all 12 edges):** `doc-new`, `doc-open`, `doc-save`, `layer-add`,
`layer-set`, `layer-reorder`, `layer-merge-down`, `layer-mask-apply`, `doc-flatten`,
`px-crop`, `px-transform`, `px-channel`.

**Also scored:** a Generative tier of six seeded algorithmic operations (`gen-fill`,
`gen-scale`, `gen-heal`, `gen-extend`, `gen-retarget`, `gen-denoise`) and 72 Extended
operations across document, layer,
tonal, geometry, filter and selection families, weighted `0.40·core + 0.25·gen + 0.35·ext`
inside S1. The Generative tier is graded against the properties in `SPEC.md` §11, not against
the reference's bytes: its output is a search result, and an output whose exact form cannot be
fairly specified must not be graded by byte comparison. `build/EDGES.md` records what that
cost — four planted generative edges, cut. The full catalog is `build/CATALOG.md`.

Grading a 12-op core rather than all 91 is deliberate and follows the design document's
time-box guidance: a smaller graded core with a green oracle is a stronger artifact than a
larger one with a red oracle. The larger surface still does work — the agent must navigate a
genuine professional tool to find the twelve it needs, and the edges live in the parts of the
document model that the breadth makes realistic.

## 6. Provenance

`refldx` and the LDX format are original, written from scratch for this assignment. No
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
