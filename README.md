# collinear-candidate/ldx-cli — a hard-but-fair long-horizon Harbor task

An agent is given a working reference binary it may **run without limit and never read**, a
160-line specification that deliberately withholds every fact the binary can answer, and four
hours. It must reimplement 95 commands of a layered-image CLI and its byte-exact container
format, then file a machine-checked attestation of what it actually verified.

| | |
|---|---|
| **Design document** | [`collinear-candidate/ldx-cli/RUN_REPORT.md`](collinear-candidate/ldx-cli/RUN_REPORT.md) — origin, results, failure analysis, fairness audit |
| **Visual report** | [`collinear-candidate/ldx-cli/RUN_REPORT.html`](collinear-candidate/ldx-cli/RUN_REPORT.html) |
| **Verifier design** | [`collinear-candidate/ldx-cli/README.md`](collinear-candidate/ldx-cli/README.md) |
| **The task** | [`collinear-candidate/ldx-cli/`](collinear-candidate/ldx-cli/) — Harbor 0.22 format |

## Results

| | Reward |
|---|---:|
| Oracle (reference solution) | **1.000** |
| GPT-5.5-high · codex | **0.346** |
| Claude Opus 4.7-high · claude-code | **0.347** |
| No-op | **0.000** |

Both target frontier models fail substantively on the same task the oracle solves perfectly.

## Reproducing

```bash
export E2B_API_KEY=...
harbor run -p ./collinear-candidate/ldx-cli -a oracle -e e2b -o ./jobs   # must be 1.000
harbor run -p ./collinear-candidate/ldx-cli -a nop    -e e2b -o ./jobs   # must be 0.000
harbor run -p ./collinear-candidate/ldx-cli -a codex -m gpt-5.5 -e e2b -o ./jobs \
    --max-retries 2 --retry-include ConnectError
harbor view ./jobs
```

No Docker daemon required — Harbor's E2B backend builds the pinned image and runs the trial
in a sandbox.

## A note on publishing this

This repository contains `refldx` and the complete verifier — the 16 planted behaviours, the
hidden assets, and the oracle solution. That is the right shape for an assignment submission,
where the reviewer needs to see how the task is built and check that it is fair.

It does mean the task cannot serve as a blind benchmark for anyone who finds this repo. The
whole design rests on `refldx` being the only source of truth in existence and its semantics
being reachable solely by running it; a reader here can simply look them up. The property the
task denies its agents is exactly the one publication gives away.

If the task is ever to be used for evaluation, it needs a private home and a fresh reference
binary built from a modified source.
