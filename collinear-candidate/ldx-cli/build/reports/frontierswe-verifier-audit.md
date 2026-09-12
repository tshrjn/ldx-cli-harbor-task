# FrontierSWE verifier methodology vs. `ldx-cli` — research and audit

Date: 2026-09-11
Scope: read-only research of FrontierSWE v2 (published site + the public task repo, including
actual verifier source), plus a code audit of `collinear-candidate/ldx-cli`
(`tests/verify.py`, `tests/test.sh`, `task.toml`, `solution/solve.sh`, `instruction.md`,
`environment/Dockerfile`) against Harbor 0.22 source at `/Users/tusharjain/Irona/repos/harbor`.

No model-backed harbor trials were run. No API keys were used. All source URLs are listed in §7.

**One-line summary:** our verifier is close to FrontierSWE on scoring granularity and
unpredictability, and materially behind on *isolation architecture* — we run agent-authored
code as root in the same container the agent had root in, we do not separate fact-gathering
from scoring, and we have a working reward hack (ship a compressed copy of the reference) that
no current gate detects.

---

## 1. What FrontierSWE actually does

FrontierSWE v2 (Proximal Labs) is 34 long-horizon tasks, 20-hour budget, 5 trials per task,
reported mean@5 with worst@5–best@5 bands. Every task ships "a deterministic, automated
verifier — agents are scored purely on whether their code works, not on style or intermediate
steps."

The tasks are **Harbor tasks**, orchestrated by a harness called `px-eval`:

> FrontierSWE v2 tasks are [Harbor](https://github.com/proximal-labs/harbor) tasks, orchestrated
> by `px-eval`.
> - `instruction.md` — the prompt given to the agent
> - `task.toml` — task configuration (timeouts, resources, scoring thresholds)
> - `environment/` — Dockerfile, setup scripts, test harness, and initial workspace
> - `solution/solve.sh` — a reference solution (oracle)
> - `preflight/preflight_checks.sh` — environment validation checks

— <https://raw.githubusercontent.com/Proximal-Labs/frontier-swe-v2/main/README.md>

So this is a direct like-for-like comparison: same harness, same task layout. The two structural
pieces we do not have are `preflight/` and the split described in §1.1.

Caveat on sourcing: `px-eval` itself is not yet published ("Coming soon"), so the harness-side
implementation of `environment_mode = "separate"` is described in prose on the blog rather than
readable as code. Everything *task-side* is readable. Website slugs differ from repo directory
names (the site's `remotion-video-generation` is the repo's `fitness-recap-video-in-remotion`).

### 1.1 The structural move: facts and policy are separate programs

This is the single most important architectural finding, and the one with no analogue in our
task. Inside `environment/tests/` the contract is identical across every task inspected:

| File | Role |
|---|---|
| `test.sh` | Harbor entrypoint. **190 bytes, byte-identical in every task** — it does nothing but `exec python3 verify.py`. |
| `verify.py` | **Orchestration only.** Builds, runs agent code unprivileged, runs the reference, collects facts. |
| `evidence.json` | Intermediate fact file at `/logs/verifier/evidence.json`, written by `verify.py`. |
| `compute_reward.py` | **Pure scoring policy.** Reads `evidence.json`, writes `reward.json`. Runs no agent code. |
| hidden data | `hidden/` (remotion), `held_out.py` (ffmpeg), `private_annotations.csv` (snooker) |

```bash
#!/bin/bash
# Verifier entry point (Harbor runs this). All logic lives in verify.py — a readable Python pipeline
exec python3 "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/verify.py"
```

Every scorer docstring states the invariant explicitly:
- remotion: "Reads only files, never imports or executes agent code."
- ffmpeg: "Reads only files root wrote; runs no agent code."
- postgres: "It NEVER reads any agent-written file: the trusted per-test signal is `pg_regress`'s
  exit code … written by root."

The payoff: "the scorer never executes agent code" becomes a *structural* property you can point
at, not a claim. It also makes scoring re-runnable and auditable without re-executing anything.

Other tasks add more phase files, all in the same spirit: `bake_baseline.py`, `verifier_driver.c`
(ffmpeg); `perturb_suite.py`, `source_scan.py`, `reset_pg.py`, `scored-tests.txt` (postgres);
`anticheat.py`, `noexec_run.c`, `reset_lua.py`, `reward_io.py` (lua).

Canonical paths (remotion `verify.py`): agent workspace `/app`, verifier `/root/tests` (root-only
0700), outputs `/logs/verifier`, agent logs `/logs/agent`.

### 1.2 `task.toml`: the clean-room switch and a non-root agent

Full remotion config, verbatim
(<https://raw.githubusercontent.com/Proximal-Labs/frontier-swe-v2/main/tasks/fitness-recap-video-in-remotion/task.toml>):

```toml
schema_version = "1.3"

[[artifacts]]
source = "/app/"
exclude = ["**/.git/**", "**/node_modules/**", "**/__pycache__/**", "**/out/**", "**/frame_*.png"]

[agent]
timeout_sec = 72000.0
user = "agent"                 # <-- the agent is NOT root

[verifier]
timeout_sec = 7200.0
environment_mode = "separate"  # fresh container on the captured /app; reference + agent render on the SAME host

# px-eval AUTO-manages this block on upload (verifier image + mirrored resources). Leave empty.
[verifier.environment]
cpus = 8
memory_mb = 16384
storage_mb = 20480

[environment]
network_mode = "allowlist"
build_timeout_sec = 3600.0
cpus = 8
memory_mb = 16384
storage_mb = 20480
```

Two details worth carrying over:
- **`[agent] user = "agent"`** — a non-root agent user is set in `task.toml`, not just in the
  Dockerfile. This matches the blog's "Non-root agent user with protected filesystem paths."
- The `[[artifacts]]` `exclude` list strips build junk **and scored outputs** (`**/frame_*.png`;
  ffmpeg excludes `**/libswscale_candidate.so`). The agent's *build products* deliberately do not
  cross the boundary — only its sources do.
- ffmpeg's toml carries a load-bearing warning: "px-eval AUTO-manages this block on upload. Must
  exist even when empty, **or the separate verifier falls back to `/tests` and never runs**." That
  is a px-eval behaviour, not Harbor's, but it is a good reason to declare
  `[verifier.environment]` explicitly rather than relying on the implicit fallback.
- `[metadata] oracle_reward_threshold = 0` (ffmpeg) is the only scoring-adjacent toml key. Despite
  the README's claim that `task.toml` holds "scoring thresholds", **every threshold is a named
  constant in `compute_reward.py`** with a comment explaining its calibration.

### 1.3 Scoring: graded distance, externally fixed denominators, calibrated constants

Nothing is bare pass/fail, and every magic number is justified in a comment.

**Remotion (video + audio):**

```python
KV = 0.1               # visual MSE e-folding (sharp term: the pixel-exact target)
WC, TC = 0.12, 1000.0  # coarse visual term: weight + e-folding, bug-calibrated
KA = 12.0              # audio: rel-MSE e-folding (rel = mse / var(ref)), bug-calibrated
WV, WA = 0.85, 0.15
```

```python
closeness.append((1 - WC) * math.exp(-KV * mse) + WC * math.exp(-mse / TC))
...
denom = max(n_ref, n_agent)
return sum(closeness) / denom, ...
```

audio: `math.exp(-KA * mse / var) * length_ratio`.

The calibration rationale is the instructive part:

> The sharp term is the target (pixel-exact pays 1.0); the small coarse term separates orders of
> magnitude below it (bug-calibrated: near-miss-everywhere rebuild mse~850 → ~0.05, scene
> structure only mse~3900 → ~0.002, blank mse>7000 → ~1e-4) **without opening a constant-fill
> floor.** Scoring every frame means in-between animation counts; a missing, symlinked, or
> escaping agent frame counts 0, and extra agent frames dilute the mean.

> KA=12 calibrated against injected source bugs: music-only/no-SFX rel=0.20 → 0.09, wrong volume
> envelope rel=0.75 → ~0, approximate placement rel=0.04 → 0.63

**ffmpeg/swscale (performance, hard correctness gate):**

```python
"""reward = correctness x (2 ** clamp((s - 1) / (FULL_CREDIT_SPEEDUP - 1), 0, 1) - 1)

s is the geometric mean of per-workload work ratios (reference / submission, >= 1); s <= 1 -> 0.
Correctness is a hard gate: wrong pixels or reaching FFmpeg is worth nothing however fast.
"""
FULL_CREDIT_SPEEDUP = 20.0
MIN_MEASURED_FRACTION = 0.75
```

**Snooker (regression):**

```python
ZERO_SCORE_RMS_M = 0.85
MAX_BALL_ERROR_M = math.hypot(TABLE_LENGTH_M, TABLE_WIDTH_M)
# score = max(0.0, min(1.0, 1.0 - rms / ZERO_SCORE_RMS_M))
```

> The 0.85 m threshold puts frozen data-free baselines at zero (example-derived constant layout:
> 0.91 m RMS; random in-bounds: 1.35 m RMS) while preserving a gradient for predictions that beat
> them. Per-ball saturation uses the table diagonal so omission equals the worst physically
> meaningful match.

**Postgres-on-SQLite (pass fraction):**

> reward = clamp01(pass_fraction) over a SINGLE all-public scored slice … the denominator is
> ALWAYS "what the real PostgreSQL 18.3 passes under this harness" (reference-counts.json,
> measured at image build over the MUTATED scoring suite), **never the run's own totals** — a
> skipped or timed-out test counts 0 against that fixed denominator, never "excluded".

**Three rules fall out:**

1. **Correctness gates zero; quality is graded.** Gates mean "not a valid submission", never
   "a weak submission".
2. **Denominators are fixed externally.** `max(n_ref, n_agent)` for frames; reference-PG pass
   counts for postgres; `expected_benchmarks` for coverage. A run must never be able to shrink
   its own denominator by failing.
3. **Constants are calibrated against injected bugs**, with the bug class and the score it earns
   written in the comment.

**Reward payload** — flat `dict[str, float|int]`, exactly as Harbor requires, plus `reward.txt`:

```python
def write_reward(outdir, reward, valid, detail):
    """Flat numeric reward.json (harbor parses dict[str, float|int]) + reward.txt."""
    reward = round(max(0.0, min(1.0, reward)), 6)
    flat = {"reward": reward, "valid": int(valid)}
    for key, value in detail.items():
        if isinstance(value, (int, float)):
            flat[key] = round(float(value), 6)
```

Per-check keys: `input_{i}`, `input_{i}_visual`, `input_{i}_audio`, `ref_failed` (remotion);
`speedup, correctness, measured, expected, build_ok, provenance_ok, wall_ratio, flagged`
(ffmpeg); `reward, valid, pass_rate, passed, attempted, tests_scored, tests_completed, restarts,
initdb_ok, server_started` (postgres). Rich nested detail goes to a second file
(`reward_details.json`) — never into `reward.json`.

**`valid` is a first-class field, separate from `reward`.** This is how infrastructure faults are
kept off the agent's record:

- remotion: "If any reference render is missing frames or audio, the run is infrastructurally
  broken: **reward 0, valid=0 (retried, not zeroed).**"
- ffmpeg distinguishes fault ownership explicitly:
  ```python
  # Anything the submission caused is charged as no speedup and still counted toward
  # coverage, so it cannot delete its own worst results from the mean ...
  # Only a genuine tooling fault is excluded, because that one is ours, not the submission's.
  # An explicit flag the verifier sets, never a substring of the error text.
  if m.get("harness_fault"):
  ```
- postgres: "a build failure, no binary, a non-ELF binary — these are real verdicts on the
  artifact, so **valid=1**. Verifier-side failures (no evidence, suite couldn't stage, reference
  missing) score 0 with **valid=0** (infra, retry)."

**Never-error guarantee** (remotion `verify.py`):

```python
if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
    finally:
        if not (VDIR / "reward.json").exists():
            write_invalid()
        sys.exit(0)
```

### 1.4 The clean room: where the reference lives and what keeps the agent out

**Primary mechanism: `environment_mode = "separate"`.** The blog states it plainly:

> "the agent's container is stopped, a fresh verifier boots from a pinned image, and the entire
> filesystem is clean."
> "Pre-written reward files, mutated binaries, planted tests, background processes: all gone by
> construction."

The remotion toml comment adds the constraint that makes this work for a render-timing task:
`# fresh container on the captured /app; reference + agent render on the SAME host`.

**Two distinct "references".** The oracle (`solution/solve.sh`) is run by px-eval for task
validation against `oracle_reward_threshold` and never appears in the agent image. The
*comparison* reference is a different artifact, and for a probe-style task it is **runnable but
not readable**:

```dockerfile
RUN sh /opt/setup/setup_user.sh          # git identity + non-root agent user
COPY setup/reference-generator /usr/local/bin/reference-generator
COPY setup/reference-daemon    /usr/local/bin/reference-daemon
RUN chmod 0755 /usr/local/bin/reference-generator && chmod 0700 /usr/local/bin/reference-daemon
RUN chown -R agent:agent /app
COPY tests/ /root/tests/
COPY reference/ /opt/reference/
RUN sh /opt/setup/bake_reference.sh
```

0755 on the client, 0700 on the daemon: the agent can *invoke* the reference, never *read* it.
Blog: "the Dockerfile creates a non-root `agent` user, all scored material lives under root-owned
mode 0700 paths"; "`test.sh` locks the reward directory and drops to the agent user before
executing any agent code."

**In-verifier sequencing is load-bearing.** Remotion `main()`, verbatim:

```python
VDIR.mkdir(parents=True, exist_ok=True)
os.chmod(VDIR, 0o700)            # lock the reward dir before any agent code runs
...
remove_probe_tool()              # SIGKILL daemon, unlink both binaries, rmtree /run/reference
...
agent_runs = agent_renders(inputs, work)
reap_agent()                     # no agent process may be alive while reference frames exist
ref_runs = reference_renders(inputs, work)
```

```python
# agent phase — agent-writable output dirs, non-root, hard timeout
os.chmod(work, 0o755)
subprocess.run(["chown", "agent:agent", str(out)], check=False)
argvs.append(["runuser", "-u", "agent", "--", "timeout", str(RENDER_TIMEOUT),
              "./render.sh", str(in_copy), str(out)])

# reap_agent()
"""No agent process may be alive while reference frames exist."""
subprocess.run(["pkill", "-u", "agent"], check=False)
time.sleep(1)
subprocess.run(["pkill", "-9", "-u", "agent"], check=False)

# reference phase — root-only output dirs
out.mkdir(); os.chmod(out, 0o700)
```

The 0755/0700 asymmetry is the crux: agent output dirs are agent-writable; reference output dirs
are root-only and only come into existence after every agent process is SIGKILLed.

**Privilege drop where `runuser` cannot reach.** ffmpeg's measurement `dlopen`s and calls the
agent's `.so` in-process, so it forks and hard-setuids:

```python
def unprivileged(fn, *args, **kwargs):
    """Call fn in a forked child permanently dropped to `agent` ...
    The measurement runs the candidate's own machine code (the thing this run judges)
    as its caller, so running it from root would be one write to /root/tests from
    scoring itself. runuser covers every other path but not this one..."""
    os.setgroups([]); os.setgid(agent.pw_gid); os.setuid(agent.pw_uid)  # no way back
```

**Confused-deputy defense on the probe daemon.** The root daemon that renders the unreadable
bundle re-checks readability *as the requesting uid*, so the agent cannot aim it at files it
cannot read:

```bash
[ "$owner" != root ] || { echo "root jobs are not accepted"; exit 2; }
runuser -u "$owner" -- test -r "$IN" || { echo "input not readable by $owner"; exit 2; }
case "$(realpath -m -- "$IN")" in /root/*|/opt/remotion/*|/etc/*|/run/reference/*) exit 2;; esac
[ "$(stat -c %U "$OUT")" = "$owner" ] || { echo "output dir must be owned by $owner"; exit 2; }
```

plus a network namespace jail with the attack written out:

```bash
# Remotion serves the (root-only) bundle to headless Chromium over an HTTP server on 127.0.0.1;
# without this the non-root agent sharing the host loopback can curl that server mid-render and
# lift the entire reference implementation.
timeout 1200 unshare -Urn -- sh -c 'ip link set lo up || true; exec node /opt/remotion/render_bundle.mjs "$@"' ...
```

This is not hypothetical paranoia — the v2 blog documents the same bug class landing in
production: "The agent used the Modal daemon Unix socket to read the hidden verifier's reference
block-device CRC files … an unprivileged agent could ask the root-owned Modal daemon to read
protected files, making user separation ineffective. The fix requires `chmod 700` on the Modal
runtime directory in every task Dockerfile."

**Preflight asserts the clean room as executable tests.** `preflight/preflight_checks.sh` runs
*before* the rollout; `blocked` means "PASS when the command FAILS":

```bash
ok      perms app-write   'touch /app/.px_probe && rm -f /app/.px_probe'
blocked perms tests-read  'ls /root/tests'
blocked perms bundle-read 'cat /root/tests/reference/bundle/* >/dev/null'
blocked perms ref-hidden  'd=$(mktemp -d) && reference-generator /root/tests/hidden/h1.json "$d" && ls "$d"/frame_0000.png'
blocked workspace no-scorer  'find /app -maxdepth 3 -name "compute_reward*.py" | grep -q .'
blocked workspace no-goldens 'find /app -maxdepth 4 -type d -name golden | grep -q .'
blocked workspace no-hidden  'find /app -maxdepth 4 -name "h[0-9].json" | grep -q .'
blocked egress   github      'curl -fsS -m 8 -o /dev/null https://github.com'
blocked timer    tamper      'echo x >> /sandbox-timer/start'
```

It writes `/logs/agent/preflight.{jsonl,json}` with per-bucket pass/fail counts (baseline, env,
infra, tools, egress, perms, workspace, timer).

### 1.5 Anti-cheat beyond isolation

**Symlink / path-escape rejection**, verbatim:

```python
def valid_agent_file(agent_dir_real, path):
    """Reject symlinks / anything resolving outside the agent's own dir (an agent file symlinked to
    the reference path would otherwise let the root scorer compare the reference against itself)."""
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    return os.path.realpath(path) == os.path.join(agent_dir_real, os.path.basename(path))
```

**Provenance gates on the built artifact — the direct answer to binary smuggling.** ffmpeg:

```python
def provenance_ok(lib):
    """The library must be its own implementation, not FFmpeg reached by
    another route. Checked on the built object, not the sources: reaching
    FFmpeg means linking it, embedding its symbols/strings, or importing a
    dynamic loader."""

ALLOWED_NEEDED = frozenset(("libc.so.6", "libm.so.6", "libgcc_s.so.1", "libstdc++.so.6",
                            "libpthread.so.0", "librt.so.1", "ld-linux-x86-64.so.2", "libunwind.so.8"))
```

with, per the agent's notes: "Allowlist, not a blocklist: a blocklist of FFmpeg's names misses the
reference library renamed"; RPATH/RUNPATH rejection ("A from-scratch library needs no search
path"); a dynsym regex `\bsws_(init|get|free|scale)|\bav_image|\bav_pix_fmt`; a dynamic-loader
import ban; **byte-comparison of every object against the reference ("a renamed copy is the same
bytes")**; and **large-binary-blob detection ("conversions must be written, not carried in as
data")**.

**Build from source only.** lua's `anticheat.py`: "pre-built objects/archives/ELF **can't enter
the build at all** (`reset_lua.py` rebuilds from source), and a decode-and-exec payload is killed
at run time by the no-exec sandbox (`noexec_run.c`)." Its structural gates read the emitted binary
read-only and "never execute it", enforcing that outputs are program-specific native code rather
than a smuggled interpreter:

```python
VM_ENGINE_STUB_ONLY = frozenset({"luaV_execute", "luaV_finishOp", "luaY_parser",
                                 "luaU_undump", "luaU_dump"})
CAPI_INIT_ALLOWED = {"luaL_newstate", "luaL_openlibs", "luaL_checkversion_",
                     "lua_close", "lua_newthread", "lua_newstate"}
STUB_SIZE_MAX = 256
```

**Structural mutation over hiding.** `perturb_suite.py` (postgres):

> Perturb the scored PostgreSQL regression scripts to defeat verbatim training-data memorization.
> … This tool rewrites each SCORED script by consistently renaming its SCRIPT-LOCAL,
> SCRIPT-CONFINED identifiers … Renames are **LENGTH-PRESERVING** … which lets `gate` prove
> faithfulness by reverse-mapping the regenerated output back and comparing it byte-for-byte to
> upstream.

Safety conditions: rename only CREATE targets local to that script and absent from every other
script; never inside strings/comments/quoted identifiers; a real SQL parser (pglast/libpg_query)
extracts the targets; **deterministic** (`sha256(script|old)`). Two-phase, self-validating: keep a
perturbation only if the reverse-mapped output equals upstream expected, else revert that test to
verbatim. The design statement:

> the agent develops against the full public un-mutated suite + expected in `/app`, but scoring
> runs a semantic-preserving MUTATED variant … kept root-only in `/tests` — **overfit is defended
> by the mutation, not by hiding tests.**

**Held-out instances, same families.** ffmpeg's `held_out.py`: "Benchmark workloads NOT shipped in
the agent's tree: same conversion families and size classes as the public set but different
dimensions/format pairs."

**Other measures:** submission size caps ("prevent baked frame dumps from masquerading as
executable code", Flight-Sim); implausibility auto-zero (single-workload speedup > 2.5×,
Cranelift); "package-boundary violation zeroes the result" and "anything shadowing the engine is
dropped" (Qubit Routing); a hard `linearity` band that catches work-skipping (below); egress
blocking asserted by preflight, including asset upstreams ("the agent must not re-source or
reverse-search the pack"); and a post-hoc audit layer in v1 ("a trial flagged by the post-hoc
audit (`scoring/anticheat.json`) scores 0 regardless").

### 1.6 Verifying what cannot be byte-compared

| Modality | Task | Technique |
|---|---|---|
| Rendered video | remotion, Flight-Sim | per-pixel MSE over **every reference frame**, exponential squash (`exp(-0.004·MSE)` on Flight-Sim; two-slope on remotion), denominator `max(n_ref, n_agent)` |
| Audio | remotion | `exp(-KA·mse/var(ref)) · length_ratio` — normalizing by reference variance makes the constant task-independent |
| CPU work | ffmpeg, Cranelift | **deterministic simulated instruction counting**, not wall clock |
| Search/algorithm output | Qubit Routing | **independent re-simulation on a pristine engine**; validity asserted ("zero unless the re-simulated schedule is valid and completes every gate"), quality scored against calibrated baselines |
| Unordered predictions | snooker | colour matching + **minimum-cost Hungarian assignment** for indistinguishable items, then RMS with a per-item cap |
| Test-suite semantics | postgres | pass fraction against an externally fixed reference denominator, on a mutated suite |
| Compiled binaries | lua | structural ELF assertions (symbols, section digests, size gaps) instead of output comparison |

**The instruction-counting detail is worth reading.** `environment/performance/insn_costs.py` is a
**230,628-byte** per-opcode cost table.

```python
"""wCEst = sum(cost[opcode] * dynamic_count[opcode]) + 17 * min(mispredicts, 2% * branches)"""
TARGET = "Intel Xeon Scalable 4th Gen (Sapphire Rapids) / Golden Cove core"
MISPREDICT = 17
MAX_MISPREDICT_RATE = 0.02
```

run under `valgrind --tool=callgrind --dump-instr=yes --branch-sim=yes`, attributed to the
candidate `.so` only. Deterministic and machine-pinned, so results reproduce. The model-vs-reality
risk is handled by **flagging for review, not scoring**:

```python
detail["flagged"] = int(s > 1.05 and wr < 0.95)
# f"model says {s:.3f}x less work but native runtime is {wr:.3f}x - review"
```

and by a **linearity check** that catches caching/no-op'ing — each workload is measured at two
iteration counts:

```python
LINEARITY_MIN, LINEARITY_MAX = 0.85, 1.15
# f"cost is not proportional to the number of conversions (second span measured {lin:.3f} of the
#  first) - conversions were skipped"
```

Aggregation is a **geometric mean** over workloads, so no single cherry-picked workload dominates.

### 1.7 The self-check tool

Blog: "every workspace contains a documented **self-check tool** whose output correlates
monotonically with the verifier score."

Remotion's is a three-layer design, and the important part is how it differs from the scorer:

| | `mse.py` (agent's self-check) | `compute_reward.py` (verifier) |
|---|---|---|
| Inputs | 3 public samples in `/app/samples/` | 6 hidden inputs in `/root/tests/hidden/` |
| Iterates over | frames **the agent rendered** | frames **the reference rendered** |
| Output | raw per-frame MSE, `identical` count, worst frame, `mean_mse`, `length_ratio` | a single 0–1 reward via the two-slope exponential |
| Weighting | none — audio and video side by side | `0.85·visual + 0.15·audio` |
| Symlink checks | none | `valid_agent_file()` |
| Trim mode | `--seconds N` for fast iteration | full renders only |

It reports **the raw quantity the reward is a monotone function of** — MSE, length ratio, frame
counts — without exposing the reward curve, the weights, or the hidden inputs. Its terminal verdict
is binary ("Exact match on every compared frame" / "Not a match yet — N/M frames identical"), so
the agent cannot hill-climb the scoring function itself. And it is destroyed before scoring:
`remove_probe_tool()` SIGKILLs the daemon, unlinks both binaries and `rmtree`s `/run/reference`
before any agent code runs in the verifier container.

---

## 2. Point-by-point comparison with `ldx-cli`

Ours: `tests/test.sh` → `tests/verify.py`, suites S0–S6, reward
`0.30·functional + 0.30·constraint + 0.20·robustness + 0.20·artifact`, zeroed only by an S0
integrity gate. Reference `tests/refldx` (mode 0700) runs side by side with `/app/src/ldx` on
identical inputs; no expected outputs stored.

| Dimension | FrontierSWE | `ldx-cli` | Verdict |
|---|---|---|---|
| Facts/policy split (`verify.py` → `evidence.json` → `compute_reward.py`) | **yes, universal** | no — one script gathers facts, runs `make`, and computes the reward | **Gap (major, architectural).** |
| Scorer never executes agent code | structural invariant, stated in every docstring | the same root process runs `make` and scores | **Gap (major).** |
| Fine-grained reward payload | flat `dict[str,float\|int]` + per-check keys + a separate `reward_details.json` | flat dict with 4 components, 6 suite scores, 16 `edge_NN` booleans, tier rates; full diagnostics in `report.json` | **Parity, arguably better.** |
| Partial credit | graded everywhere, thresholds avoided | S1 tiered `0.40·core + 0.25·gen + 0.35·ext`, linear; S2 = edge fraction; S3/S4/S5 pass rates; S6 `0.4·schema + 0.6·(1−overclaim)` | **Parity**, one exception below. |
| Per-item granularity | per-frame MSE, per-ball distance, per-test pass | S1 scores an *operation* all-or-nothing across its cases; `mean_case_rate` computed but **not scored** | **Gap (minor).** 11/12 scores the same as 0/12. |
| Externally fixed denominators | explicit rule (`max(n_ref,n_agent)`, reference-PG counts) | S1/S3/S4/S5 denominators come from the reference side | **Parity.** |
| Constants calibrated against injected bugs | every magic number, with the bug and its score in the comment | edge divergences measured in `build/EDGES.md`; scoring weights not bug-calibrated | Minor gap. |
| `valid` / `harness_fault` separate from `reward` | first-class, drives retry vs. score | `verifier_crash` exists but a reference-fixture failure still yields **reward 0.0** | **Gap (moderate).** |
| Never-error guarantee | try/except/finally → `write_invalid()`, `sys.exit(0)` | `test.sh` writes a fallback `reward.json` and `exit 0` | **Parity.** |
| Calibrated baselines published | snooker states constant-layout and random baselines | `RUN_REPORT.md`: oracle 1.0 ×7 byte-identical, nop 0.0, naive 0.238, naive-plus 0.481 | **Parity.** |
| Gates zero only for invalidity | correctness gates zero; weak work graded | S0 is anti-cheat only; explicitly "no scope gate" | **Parity, correctly reasoned.** |
| Separate verifier container | `environment_mode = "separate"` on every task | **not set** → Harbor 0.22 default **shared** | **Gap (major).** §5 |
| Non-root agent | `[agent] user = "agent"` + Dockerfile `agent` user | agent runs as **root**; no `USER` in `environment/Dockerfile`; no `[agent] user` | **Gap (major).** |
| Agent code de-rooted during verification | `runuser -u agent`, and a hard `setuid` fork where `runuser` can't reach | `make` and the agent binary run as **root** | **Gap (major).** |
| Reward dir locked before agent code | `os.chmod(VDIR, 0o700)` first statement of `main()` | not locked; agent-writable throughout its phase | **Gap (moderate).** |
| Agent processes reaped before reference output exists | `pkill -u agent` then `pkill -9`, with reference dirs at 0700 | not done | **Gap (major in shared mode).** |
| Probe tool removed before grading | `remove_probe_tool()` unlinks binaries, kills daemon, `rmtree`s spool | S0 deletes byte-identical copies of `refldx` under `/app` | **Parity.** |
| Reference readable by the agent? | **no** — runnable via a 0755 client, the 0700 daemon and bundle are unreadable | **yes** — `/app/refldx` is mode 0755 and fully readable | Deliberate difference; see §4.3. |
| Graded artifact rebuilt from pristine base + whitelisted agent *sources* | **yes on every task** (`reset_lua.py`, "pristine sources plus only the agent's `.rs`/`.isle` edits", `[[artifacts]] exclude` strips build products) | **no** — we grade whatever `make -C /app/src` leaves, with no restriction on `/app/src` contents | **Gap (critical).** §4.1 |
| Provenance gates on the built binary | DT_NEEDED allowlist, RPATH ban, dynsym regex, byte-compare vs reference objects, blob detection | one 64-byte probe slice searched in `/app/src` | **Gap (critical).** §4.1 |
| Symlink / path-escape rejection | `valid_agent_file()` realpath check | symlinks in `/app/src` fail S0; `OUT.ldx` symlink fails the case | **Parity.** |
| Size cap | 16 MB | 16 MB (`SRC_SIZE_CAP`) | Parity on the number, but see §4.1 — 16 MB is far too loose for our artifact. |
| Test-suite unpredictability | structural mutation (`perturb_suite.py`) + held-out instances in the same families | S1 asset sampling and S3 fuzz seeded from `os.urandom` at scoring time; 40 hidden documents | **Parity, arguably better.** |
| Self-check tool | monotone in the reward, different inputs/domain, raw metrics not the curve | `compare.sh` scores naive-plus at **95%** vs a real reward of **0.481** | **Gap (minor).** |
| Preflight | `preflight/preflight_checks.sh` with the `blocked` idiom, writes `/logs/agent/preflight.json` | none in-task; `build/` tooling run by hand | Minor gap. |
| Egress policy asserted | preflight `blocked egress …` per host | `verify.py` probes DNS+HTTPS and records it in `report.json` (diagnostic, unscored) | **Parity.** |

---

## 3. Gap list, ranked

1. **CRITICAL — the graded binary is agent-supplied, not rebuilt from agent *sources*.** §4.1.
   A working 1.0-reward hack exists today, verified empirically.
2. **MAJOR — shared verifier container.** §5. `environment_mode` unset → Harbor default `shared`;
   PRD v2 §11 says `separate`. We are not compliant with our own PRD.
3. **MAJOR — the agent is root, and the verifier runs agent-authored code as root.** §4.2.
   The `chmod 0700` on `/tests/refldx` is doing no work whatsoever.
4. **MAJOR — `strace`/`ldd` gates fail open.** §4.3.
5. **MAJOR — the exec gate matches a path substring, on one fixed command.** §4.3.
6. **MAJOR — no facts/policy split.** §4.4. Everything above is harder to fix, and harder to
   *assert*, because one root process does collection and scoring.
7. **MODERATE — harness defects are charged to the agent.** No `valid` / `harness_fault` field.
   §4.5.
8. **MODERATE — the reward directory is agent-writable, and host-mounted, during the agent phase.**
   §4.6.
9. **MODERATE — `tests/` uploads as an overlay, not a replacement** (shared mode only), and we ship
   a stale `tests/__pycache__/`. §4.7.
10. **MINOR — S1 scores operations all-or-nothing**, discarding per-case partial credit and adding
    sampling variance. §4.8.
11. **MINOR — `compare.sh` is a poorly calibrated self-check** (95% vs a true 0.481). §4.9.
12. **MINOR — deprecated config keys, missing `[task]` block, no `[agent] user`.** §4.10.
13. **MINOR — no `preflight/`.** §4.11.

Items 1–6 are the ones that change whether a determined agent can beat the verifier.

---

## 4. Gaps in detail

### 4.1 CRITICAL — the graded binary is agent-supplied

`gates()` deletes only files under `/app` that are **byte-identical** to `refldx`. It then runs
`make -C /app/src` and grades whatever executable sits at `/app/src/ldx`. It never deletes a
pre-existing `/app/src/ldx`, never deletes stale objects, and never restricts what may live in
`/app/src`. The "embedded reference" check searches for exactly one 64-byte slice from the middle
of `refldx`.

Exploit, effective today, in shared **or** separate mode:

```makefile
# /app/src/Makefile
ldx: blob.bin
	python3 -c "import zlib;open('ldx','wb').write(zlib.decompress(open('blob.bin','rb').read()))"
	chmod +x ldx
```

with `blob.bin` = zlib of `refldx`. Every S0 gate passes: byte-identity sweep — no match;
embedded-slice probe — no match; size cap — fine; symlinks — none; `make` — succeeds;
`ldx_exists` — yes; strace `execve` filter — it matches the substring `"refldx"` in the executed
*path*, and the path here is `/app/src/ldx` (and the decompression happened at build time, so the
traced run shows no extra `execve` at all); network — none. **Reward 1.0 for shipping the
reference.**

Verified empirically against the real `tests/refldx`: the binary is 791,328 bytes;
`zlib.compress(refldx, 9)` is **354,498 bytes**, round-trips exactly, has a different SHA-256,
does **not** contain the 64-byte probe slice, and is 46× under the 16 MB cap.

`instruction.md` forbids this in prose ("may not execute, link against, read, copy or embed
`refldx`") and `build/audit_traces.py` greps trajectories for `cp|base64|dd … refldx` — but that
is a post-hoc, human-readable audit. The reward is unaffected.

FrontierSWE blocks exactly this, in two independent layers, and names the same threat:

- **Rebuild from source only.** lua: "pre-built objects/archives/ELF **can't enter the build at
  all** (`reset_lua.py` rebuilds from source)". Cranelift: "pristine Wasmtime sources plus only
  the agent's `.rs`/`.isle` edits". Qubit routing: "a pristine baked copy of the engine plus only
  the agent's own `.py` files (anything shadowing the engine is dropped)". And the artifact
  `exclude` lists strip build products so they never cross the container boundary at all.
- **Provenance gates on the emitted object**: `ALLOWED_NEEDED` allowlist, RPATH/RUNPATH rejection,
  dynsym regex, **byte-comparison of every object against the reference** ("a renamed copy is the
  same bytes"), and **large-binary-blob detection** ("conversions must be written, not carried in
  as data").

**Fix — do all four:**

1. **Whitelist the deliverable and rebuild from scratch.** Before `make`, delete everything in
   `/app/src` that is not a recognized source file, plus any pre-existing `ldx` and objects:

   ```python
   SRC_ALLOWED_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".sh", ".mk"}
   SRC_ALLOWED_NAMES = {"Makefile", "makefile", "GNUmakefile"}
   dropped = []
   for p in sorted(SRC.rglob("*"), reverse=True):
       if p.is_file() and not p.is_symlink():
           if p.suffix not in SRC_ALLOWED_SUFFIXES and p.name not in SRC_ALLOWED_NAMES:
               p.unlink(); dropped.append(str(p))
   g["dropped_non_source"] = dropped   # reported, not fatal — mirrors "anything shadowing is dropped"
   ```
2. **Blob detection on every surviving source file.** Reject (or drop) any file with more than
   ~5% bytes outside printable ASCII + whitespace, or any single string/array literal over a few
   KB. This is FrontierSWE's "must be written, not carried in as data".
3. **Cut the size cap.** A from-scratch `ldx` is a few hundred KB of C; 16 MB is three orders of
   magnitude of headroom that only helps a smuggler. 2 MB is still generous.
4. **Provenance check the built binary**: compare `/app/src/ldx` against `refldx` on section
   digests and symbol sets, not just whole-file SHA-256, and reject a static binary whose `.text`
   matches the reference's. Also `exclude` the build products from the artifact set once we move
   to separate mode (§5.4).

And state the whitelist in `instruction.md`, so it is a published contract rather than a trap.

### 4.2 MAJOR — root agent, root build, meaningless 0700

The agent runs as root: `environment/Dockerfile` declares no `USER` and `task.toml` has no
`[agent] user`. `make -C /app/src` then runs an arbitrary agent Makefile as **root** from the same
Python process that later computes and writes the reward — true in shared *and* separate mode.

Both `test.sh` and `verify.py` `chmod 0700 /tests/refldx`. Against a root process this buys
**nothing**. The same is true of `/tests` and `/logs/verifier`.

FrontierSWE: `[agent] user = "agent"`; `runuser -u agent --` for every agent invocation; and a
hard `setuid` fork ("real, effective and saved — there is no way back") for the one call path
`runuser` cannot cover, with the reasoning spelled out: "running it from root would be one write
to `/root/tests` from scoring itself."

**Fix:** create an unprivileged user in the image, set `[agent] user`, and run `make` **and every
agent-binary invocation** as that user (`runuser -u ldxbuild -- …` or `setpriv --reuid/--regid
--clear-groups`). Keep `/tests` and `/tests/refldx` root-owned 0700, and `chmod 0700
/logs/verifier` as the first statement of the verifier. Only then do the existing mode bits mean
anything.

Mirror their directory asymmetry: the agent binary's scratch dirs agent-writable, the reference's
scratch dirs root-only 0700.

### 4.3 MAJOR — the exec/network gates fail open and match on a name

```python
if shutil.which("strace"): ...
else: note("strace not available; exec trace skipped")     # exec_clean stays True
```

In a shared container the agent is root and can `rm /usr/bin/strace` during its phase, silently
disabling the exec **and** network gates. Same for `ldd`.

```python
bad = [e for e in execs if "refldx" in e.lower()]
```

An agent that reconstructs the reference at `/tmp/h` and `execve`s it is not detected — the check
is on the path *name*, not the binary's identity. And `strace` runs on exactly one sample command
(`doc-flatten`), so a deliverable can behave under that one argv and shell out everywhere else.

FrontierSWE's equivalent is an **allowlist on the built object**, not a name blocklist, with the
rationale stated: "Allowlist, not a blocklist: a blocklist of FFmpeg's names misses the reference
library renamed."

**Fix:** missing `strace`/`ldd` must set `exec_clean = False` (or raise an infra fault, §4.5),
never pass. Flag *any* `execve` outside a small allowlist of system paths. Hash every executed
path and compare against `refldx`. Trace a random sample of S1/S3 cases, not one fixed command.

### 4.4 MAJOR — no facts/policy split

Our `verify.py` gathers evidence, runs `make`, runs both binaries, and computes and writes the
reward, all in one root process. FrontierSWE splits this into `verify.py` (facts →
`evidence.json`) and `compute_reward.py` (policy, "reads only files, never imports or executes
agent code").

The split is not cosmetic: it is what lets them *assert* the scorer never touches agent code, and
it makes scoring re-runnable without re-executing anything (useful for regrades, and Harbor 0.22
has `trial/regrade.py`).

**Fix:** have `verify.py` write `/logs/verifier/evidence.json` — per-case results, gate evidence,
seeds, the network probe, the S6 attestation comparison — and add a `compute_reward.py` that reads
only that file and writes `reward.json`. Our `report.json` is already 80% of `evidence.json`; the
work is mostly moving the arithmetic out of `main()`.

### 4.5 MODERATE — harness defects are charged to the agent

`ref_make` raising (a reference fixture that fails) propagates to the `except` in `main()`, giving
`verifier_crash=1` and **reward 0.0**. FrontierSWE treats this as ours, not the agent's:
"reward 0, **valid=0** (retried, not zeroed)"; "Only a genuine tooling fault is excluded, because
that one is ours, not the submission's. An explicit flag the verifier sets, never a substring of
the error text."

Note their careful line: a **build failure is the agent's** (`valid=1`, a real verdict); a
**reference-side failure is ours** (`valid=0`, retry).

**Fix:** add `valid` and `harness_fault` to `reward.json`. Wrap each fixture/suite independently: a
reference-side failure drops that case from the denominator and sets `harness_fault`; an
agent-side failure costs score with `valid=1`. Keep the existing never-error guarantee in
`test.sh`.

### 4.6 MODERATE — the reward directory is agent-writable, and host-mounted

`Trial._agent_env_mounts` (`trial.py:1629-1641`) bind-mounts the **host** trial verifier directory
into the **agent** container at `/logs/verifier` for the whole agent phase:

```python
ServiceVolumeConfig(
    type="bind",
    source=self.paths.verifier_dir.resolve().absolute().as_posix(),
    target=str(self.agent_env_paths.verifier_dir),
)
```

On mounted backends (Docker) the agent writes directly into the host directory Harbor later reads
`reward.json` from. Several backends also create `/logs/{agent,verifier}` `chmod 777` at
environment start (`environments/{ack,novita,opensandbox,…}.py`; the generic
`_ensure_dirs_command` appends `chmod 777`); on E2B the directory comes from our own Dockerfile
(`mkdir -p /logs/agent /logs/verifier`, root-owned) — but the agent is root, so it is writable
either way. In shared mode nothing wipes it before verification.

FrontierSWE locks it first thing: `os.chmod(VDIR, 0o700)` is the opening statement of `main()`,
and the blog says "`test.sh` locks the reward directory and drops to the agent user before
executing any agent code."

**Fix:** `rm -rf /logs/verifier && mkdir -m 0700 -p /logs/verifier` as the first line of
`test.sh`. In **separate** mode Harbor already does `empty_dirs([env_paths.verifier_dir],
chmod=True)` before the verifier runs (`trial.py:704`) — another concrete argument for separate.

### 4.7 MODERATE — `tests/` uploads as an overlay; we ship stale bytecode

`Verifier.verify()` calls `environment.upload_dir(source_dir, target_dir="/tests")`, and
`upload_dir` writes each source file individually (`e2b.py:324-349`); it does **not** wipe the
target. Files the agent pre-created under `/tests` that are not in our `tests/` tree survive.
Combined with `sys.path.insert(0, "/tests")` in `verify.py`, any import path we do not ship is
agent-controllable.

Related: `tests/__pycache__/` exists on disk right now (`ldxfmt.cpython-311.pyc`,
`opspec.cpython-311.pyc`, `verify.cpython-311.pyc`) and ships with the rest of `tests/`. Stray
`.pyc` files beside our modules are exactly the wrong thing to ship — a PEP 552 "unchecked hash"
`.pyc` is used without validating its source. Ship a clean `tests/` and run `python3 -B`.

### 4.8 MINOR — S1 scores operations all-or-nothing

An operation passes only if *every* one of its `ASSETS_PER_OP = 3`-sampled cases matches;
`mean_case_rate` is computed and reported but not scored. Under a scoring-time random sample this
also adds variance — the same implementation can flip an op between pass and fail run to run.

**Fix:** blend, e.g. `0.5·op_pass_rate + 0.5·mean_case_rate` inside each tier, keeping the
`0.40/0.25/0.35` weights. That is FrontierSWE's per-item graded credit applied to our structure,
and it reduces variance without weakening discrimination.

### 4.9 MINOR — `compare.sh` is a poorly calibrated self-check

The blog is explicit that self-check output should "correlate monotonically with the verifier
score." Ours reports 95% for an implementation whose real reward is 0.481 (`README.md` §4,
`RUN_REPORT.md`). An agent that trusts it stops early with a badly wrong picture.

Use the remotion design (§1.7) as the template: report the **raw quantity** (cases matched / cases
run, per command), over public assets, iterating over the *reference's* case list — not a
percentage that reads like a score, and not the reward curve or the tier weights.

### 4.10 MINOR — config hygiene

`task.toml` uses `version = "1.0"` (Harbor renames it to `schema_version`, `config.py:826`) and
`[environment] allow_internet`, which Harbor 0.22 flags as deprecated: "The 'allow_internet' field
is deprecated. Use `[environment].network_mode` instead." (`config.py:509-512`). There is no
`[task]` block and no `[agent] user`. None of this breaks a run, but it emits warnings and will
rot. Move to `schema_version = "1.4"`, `network_mode`, a `[task]` section, and `[agent] user`.

### 4.11 MINOR — no `preflight/`

FrontierSWE ships `preflight/preflight_checks.sh` per task, encoding the isolation contract as
executable assertions with a `blocked` idiom (pass when the command fails) and writing
`/logs/agent/preflight.json`. Our equivalents (`build/check_compare.py`, `build/naive_task.py`,
`build/find_edges.py`) live outside the task and are run by hand. A small `preflight/` would catch
image drift loudly — e.g. `blocked perms tests-read 'ls /tests'`, `blocked workspace no-hidden
'find /app -name "h[0-9][0-9]_*"'`, `ok tools 'command -v strace gcc make'`.

---

## 5. Shared vs. separate verifier environment

### 5.1 What Harbor 0.22 actually does

Confirmed by reading `/Users/tusharjain/Irona/repos/harbor` (`pyproject.toml` → `version = "0.22.0"`).

`src/harbor/models/task/verifier_mode.py`:

```python
def _resolve_mode(verifier):
    if verifier.environment_mode is not None:
        return verifier.environment_mode
    if verifier.environment is not None:
        return VerifierEnvironmentMode.SEPARATE
    return None

def resolve_task_verifier_mode(task_cfg):
    return _resolve_mode(task_cfg.verifier) or VerifierEnvironmentMode.SHARED
```

Our `task.toml` sets neither `[verifier] environment_mode` nor `[verifier.environment]`.
**Effective mode: `shared`.** The verifier runs in the very container the agent had root in for
four hours. PRD v2 §11 specifies `environment_mode = "separate"`.

In **shared** mode, `Verifier.verify()` (`verifier.py:165-230`) uploads `tests/` over `/tests` (an
overlay, §4.7), `chmod +x`s the test script, and `exec`s it as the environment's default user —
root.

In **separate** mode (`trial.py:674-807`):
- artifacts are collected from the agent env, the agent env is stopped, and a **new** container
  is started;
- the verifier container's **Docker build context is `tests/`** (`_verifier_env_build_context`
  returns `self.task.paths.tests_dir`), with `skip_tests_upload=True` — so `tests/` must contain a
  `Dockerfile` that `COPY`s the verifier into the image; nothing is uploaded at run time;
- `empty_dirs([env_paths.verifier_dir], chmod=True)` wipes `/logs/verifier` before anything runs;
- collected artifacts are re-materialized "to their original `source` path"
  (`artifact_handler.py:218-224`), so `artifacts = ["/app/src"]` lands back at `/app/src`;
- the convention dir `/logs/artifacts` is carried across;
- **nothing else from the agent container exists.** Harbor's own example asserts exactly this:
  `if [ -e /tmp/agent-only.txt ]; then fail "ambient agent-only file leaked into the verifier
  environment"; fi` (`examples/tasks/separate-verifier-environment/tests/test.sh`).

This matches what FrontierSWE describes: "the agent's container is stopped, a fresh verifier boots
from a pinned image, and the entire filesystem is clean. Pre-written reward files, mutated
binaries, planted tests, background processes: all gone by construction."

### 5.2 What an agent could do to our shared container

Every item assumes only what the agent already has: root, four hours, and the knowledge (from
`instruction.md`) that grading runs a traced comparison.

1. **Replace the interpreter.** `test.sh` is `#!/bin/bash` and invokes bare `python3`. Overwrite
   `/usr/bin/python3` with a wrapper that writes a perfect `/logs/verifier/reward.json` and exits
   0. One file. This alone defeats the entire verifier.
2. **`/etc/ld.so.preload`.** Root can install a global `LD_PRELOAD` hooking `read`/`write`/`open`
   in every dynamically linked process — `python3` and `make` included. (`refldx` is static and so
   immune, but the *comparison* is not: the agent controls what `verify.py` sees.)
3. **Delete `/usr/bin/strace` and `/usr/bin/ldd`.** Both gates fail open (§4.3), so the
   exec-of-the-reference and outbound-network checks silently vanish.
4. **Shadow the toolchain.** `make`, `gcc`, `ld` are invoked with
   `PATH=/usr/local/bin:/usr/bin:/bin`; `/usr/local/bin` sorts first and is root-writable.
5. **Pre-stage files under `/tests`.** The upload is an overlay; anything we do not ship survives,
   and `/tests` is `sys.path[0]` for `verify.py`.
6. **Pre-stage `/logs/verifier`.** Writable throughout the agent phase, and on mounted backends it
   *is* the host directory Harbor reads the reward from (`trial.py:1629-1641`). Nothing wipes it
   before a shared-mode verification.
7. **Leave a background process running.** Nothing kills agent processes. A watcher that polls for
   `/tests/refldx`, or that rewrites `reward.json` after `verify.py` exits but before Harbor's
   `download_dir` (`verifier.py:228-250`), needs no privilege escalation at all.
8. **Harvest the hidden assets.** `tests/hidden_assets/` (40 documents + PNMs + manifest) lands in
   the same container. A resident watcher can read it, run `refldx` over it, and produce exact
   expected outputs — defeating the "no stored expected outputs" property entirely.

Items 1–8 are all specific to shared mode. Note that 7 and 8 also defeat the `chmod 0700
/tests/refldx` line: the mode is applied *by the verifier*, after the upload, and the watcher is
root anyway. FrontierSWE names this exact set — "Pre-written reward files, mutated binaries,
planted tests, background processes" — as what separate mode removes by construction.

### 5.3 Recommendation

**Switch to `environment_mode = "separate"`.** It is what our own PRD v2 §11 specifies, what every
FrontierSWE v2 task does, and it removes items 1–8 above at a stroke: fresh container, verifier
baked into a pinned image, `/logs/verifier` wiped by Harbor, no agent processes, no ambient
filesystem, and `tests/hidden_assets` + `tests/refldx` never co-resident with anything the agent
touched.

It does **not** remove §4.1 (binary smuggling) or §4.2 (root build) — the Makefile still runs, and
still runs as root, verifier-side. **Do §4.1 first**: it is a complete reward hack that separate
mode does not touch. Then separate mode, then de-rooting.

### 5.4 Exactly how to configure it for Harbor 0.22

**`task.toml`:**

```toml
schema_version = "1.4"

# Carried from the agent container into the fresh verifier container, at the same absolute
# paths. Nothing else crosses the boundary. Build products are excluded so the graded binary
# is always rebuilt from the sources we whitelisted (see §4.1).
[[artifacts]]
source = "/app/src"
exclude = ["**/*.o", "**/*.a", "**/*.so", "**/ldx", "**/.git/**", "**/__pycache__/**"]

[[artifacts]]
source = "/app/conformance.json"

[agent]
timeout_sec = 14400.0
user = "agent"                    # see §4.2 — non-root agent

[verifier]
timeout_sec = 1800.0
environment_mode = "separate"

# Declare this explicitly even though Harbor would fall back to a copy of [environment]:
# it is self-documenting, and FrontierSWE's harness notes that an absent block can cause the
# separate verifier to silently fall back and never run.
[verifier.environment]
network_mode = "no-network"       # the verifier needs no egress; also stops a malicious
                                  # Makefile from phoning home during the build
build_timeout_sec = 900.0
cpus = 4
memory_mb = 8192
storage_mb = 20480
gpus = 0

[environment]
network_mode = "public"           # only so agent harnesses can self-install
build_timeout_sec = 900.0
cpus = 4
memory_mb = 8192
storage_mb = 20480
gpus = 0
```

Setting `environment_mode = "shared"` *together with* `[verifier.environment]` is a validation
error (`config.py:599-609`); setting `separate` with the block is the supported combination.

**`tests/Dockerfile`** — new file. `tests/` becomes the verifier image's build context and nothing
is uploaded at run time, so everything the verifier needs must be `COPY`d in:

```dockerfile
# Same pinned digest as environment/Dockerfile so the agent's toolchain and the verifier's
# are identical — otherwise a legitimate deliverable compiles in one and not the other.
FROM ubuntu:24.04@sha256:1e0a86e57d247923571b75e0aaf48a1449cf8c543d51fb3e07a4a7d7bfa79316
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc g++ make libc6-dev python3 strace file \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m -s /usr/sbin/nologin ldxbuild
COPY --chmod=755  test.sh        /tests/test.sh
COPY             verify.py       /tests/verify.py
COPY             compute_reward.py /tests/compute_reward.py
COPY             opspec.py       /tests/opspec.py
COPY             ldxfmt.py       /tests/ldxfmt.py
COPY             hidden_assets/  /tests/hidden_assets/
COPY --chmod=700 refldx          /tests/refldx
RUN chown -R root:root /tests && chmod 0700 /tests && mkdir -p /app /logs/verifier
```

**Other required edits:**

- `tests/test.sh`: drop `chmod 0700 /tests/refldx` (now baked); add
  `rm -rf /logs/verifier && mkdir -m 0700 -p /logs/verifier` at the top.
- `verify.py`: keep the `reference_removed` sweep over `/app` (it now sweeps only the uploaded
  `/app/src`, which is exactly right); add the whitelist rebuild from §4.1; run `make` and the
  agent binary as `ldxbuild`.
- `environment/Dockerfile`: add the `agent` user and `chown -R agent:agent /app`; keep `/tests`
  absent from the agent image entirely.
- `instruction.md`: add one line stating the grading boundary —
  *"Grading copies only `/app/src` (source files only — build products are discarded and rebuilt)
  and `/app/conformance.json` into a fresh container. Nothing else you write survives. Your
  `Makefile` must build with no network and no files outside `/app/src`."* Without this, an agent
  whose Makefile reads `/app/assets` or `/app/SPEC.md` fails for a reason it was never told.
- `.dockerignore` in `tests/` (or a packaging exclusion) for `__pycache__`.

**What it costs / what could break:**

| Item | Impact |
|---|---|
| Extra container build + boot per trial | one more image build (cached across trials) + ~10–30 s boot. Verifier body is ~6 s today, so total trial time barely moves. |
| Verifier image size | `refldx` (791 KB) + 40 hidden assets bake in instead of uploading. Negligible. |
| `tests/Dockerfile` becomes load-bearing | forgetting to `COPY` a new fixture means it is silently missing at grade time. Add a `preflight/` check (§4.11) that every path `verify.py` references exists in the image. |
| **Toolchain divergence** | **the real risk.** If the two Dockerfiles drift, a deliverable that builds for the agent fails for the verifier and scores 0 unfairly. Mitigate by pinning the same digest and the same `apt-get install` line, and re-running the oracle/naive matrix after the switch. |
| Deliverables outside `/app/src` | any agent that writes outside the artifact set loses that work. Must be stated in `instruction.md` (above). |
| Network policy | the agent env can keep `network_mode = "public"` for harness self-install while the verifier env is `no-network` — separate mode makes the two independent, which is strictly better than today's all-or-nothing `allow_internet`. |
| Re-validation | the full `RUN_REPORT.md` matrix (oracle ×7, nop, naive, naive-plus) must be re-run to confirm the oracle still hits 1.0 byte-identically. The main labour cost — a few E2B trials, **no model calls**. |

Harbor ships a ready-made runtime check for exactly this switch:
`harbor run --path examples/tasks/verifier-mode-matrix -e daytona -a oracle`, and
`examples/tasks/separate-verifier-environment/` is a complete minimal working example to copy.

---

## 6. Recommended changes, in order

1. **Whitelist-rebuild the deliverable** (§4.1): drop non-source files from `/app/src`, delete any
   prebuilt `ldx`/objects, blob-detect the surviving sources, cut the size cap to ~2 MB, and
   provenance-check the built binary against `refldx` on sections and symbols. *Closes a working
   1.0-reward hack, independent of everything else.*
2. **`environment_mode = "separate"`** with the artifact set, `[verifier.environment]`, and a
   `tests/Dockerfile` (§5.4). Re-run the oracle/nop/naive matrix.
3. **De-root everything agent-authored** (§4.2): `[agent] user`, an `agent` user in the image,
   `runuser` for `make` and every agent-binary invocation, `/tests` root-only 0700,
   `chmod 0700 /logs/verifier` first thing in `test.sh`, reference scratch dirs 0700 vs agent
   scratch dirs 0755.
4. **Fail the exec/network gates closed** (§4.3): missing `strace`/`ldd` ⇒ fail (or infra fault);
   allowlist executed paths instead of blocklisting the name `refldx`; hash-compare executed
   binaries; trace a random sample of cases.
5. **Split facts from policy** (§4.4): `verify.py` → `/logs/verifier/evidence.json` →
   `compute_reward.py` → `reward.json`. Our `report.json` is already most of `evidence.json`.
6. **Add `valid` and `harness_fault`** (§4.5): reference-side failures drop from the denominator
   and set `valid=0` (retry); agent-side failures cost score with `valid=1`.
7. **Blend S1's per-op pass rate with the per-case rate** (§4.8), `0.5/0.5` inside each tier.
8. **Recalibrate `compare.sh`** to report raw matched/run counts over the reference's case list
   rather than a score-like percentage (§4.9).
9. **Config and packaging hygiene** (§4.10, §4.7): `schema_version = "1.4"`, `network_mode`,
   `[task]` block, `[agent] user`, ship `tests/` without `__pycache__`, run `python3 -B`.
10. **Add `preflight/`** (§4.11) with the `blocked` idiom: `/tests` unreadable from the agent
    environment, no hidden assets or scorer under `/app`, toolchain present, both Dockerfiles agree
    on base digest.

Items 1–4 change whether a determined agent can beat the verifier. 5–10 are quality, fairness and
maintainability.

---

## 7. Sources

**FrontierSWE / Proximal Labs — site:**
- <https://www.frontierswe.com/>
- <https://www.frontierswe.com/blog/v2>
- <https://www.frontierswe.com/tasks>
- <https://www.frontierswe.com/tasks/remotion-video-generation>
- <https://www.frontierswe.com/tasks/opengl-scene-engine>
- <https://www.frontierswe.com/tasks/qubit-routing>
- <https://www.frontierswe.com/tasks/snooker-prediction>
- <https://www.frontierswe.com/tasks/cranelift-codegen-opt>
- <https://www.frontierswe.com/contribute> (recruiting page; no verifier-authoring guidance)
- <https://www.frontierswe.com/changelog> (minimal; no verifier/scoring entries)

**FrontierSWE — repositories:**
- <https://github.com/Proximal-Labs/frontier-swe-v2>
- <https://raw.githubusercontent.com/Proximal-Labs/frontier-swe-v2/main/README.md>
- <https://github.com/Proximal-Labs/frontier-swe-v2/tree/main/tasks>
- `tasks/fitness-recap-video-in-remotion/` — `task.toml`, `environment/Dockerfile`,
  `environment/tests/{test.sh,verify.py,compute_reward.py}`,
  `environment/setup/{reference-generator,reference-daemon}`,
  `environment/app/samples/{check.sh,mse.py}`, `preflight/preflight_checks.sh`
- `tasks/ffmpeg-libswscale-optimization/` — `task.toml`,
  `environment/tests/{verify.py,compute_reward.py,held_out.py,verifier_driver.c}`,
  `environment/performance/{performance.py,insn_pricing.py,insn_costs.py}`
- `tasks/snooker-prediction/environment/tests/compute_reward.py`
- `tasks/postgresql-18-on-sqlite/environment/tests/{compute_reward.py,perturb_suite.py}`
- `tasks/lua-native-compiler/environment/tests/{anticheat.py,reset_lua.py,noexec_run.c}`
  (all reachable as `https://raw.githubusercontent.com/Proximal-Labs/frontier-swe-v2/main/<path>`)
- <https://github.com/Proximal-Labs/frontier-swe> (v1)
- <https://raw.githubusercontent.com/Proximal-Labs/frontier-swe/main/SCORING.md> (v1 aggregation;
  no v2 equivalent exists in the repo)
- <https://www.proximal.ai/blog/frontierswe/>

Noted in search results, not used as a source for any claim: <https://github.com/datacurve-ai/deep-swe>

**Known gaps in the public record:** `px-eval` is not published ("Coming soon"), so the harness-side
implementation of separate-mode verification is prose-only; there is no v2 `SCORING.md`; there are
no prebuilt Docker images yet; and website slugs differ from repo directory names.

**Local (read-only):**
- `/Users/tusharjain/Irona/repos/evo-machines/collinear-candidate/ldx-cli/{README.md,instruction.md,task.toml,environment/Dockerfile,tests/test.sh,tests/verify.py,tests/refldx,solution/solve.sh,build/audit_traces.py}`
- `/Users/tusharjain/Irona/repos/harbor/pyproject.toml` (version 0.22.0)
- `/Users/tusharjain/Irona/repos/harbor/src/harbor/models/task/{verifier_mode.py,config.py}`
- `/Users/tusharjain/Irona/repos/harbor/src/harbor/trial/{trial.py,single_step.py,artifact_handler.py}`
- `/Users/tusharjain/Irona/repos/harbor/src/harbor/verifier/verifier.py`
- `/Users/tusharjain/Irona/repos/harbor/src/harbor/environments/{base.py,e2b.py}`
- `/Users/tusharjain/Irona/repos/harbor/examples/tasks/{separate-verifier-environment,verifier-mode-matrix}/`
