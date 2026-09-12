#!/usr/bin/env python3
"""Token and cost accounting per model trial, straight from the trajectories.

Both harnesses report usage, in different shapes:

  claude-code  a final `result` event with `usage`{input_tokens, cache_creation_input_tokens,
               cache_read_input_tokens, output_tokens} and `total_cost_usd`
  codex        a `turn.completed` event with `usage`{input_tokens, cached_input_tokens,
               output_tokens, reasoning_output_tokens} and no cost

Cache reads are broken out because they dominate an agentic run and are billed at a fraction
of fresh input; a total that hides them makes a long run look far more expensive than it is.
Where the harness reports no cost (codex, and anything run on a Claude subscription rather
than a metered key) the row shows tokens only and says so, rather than inventing a number.

Usage:  .venv/bin/python build/run_costs.py <jobs_dir> [more_jobs_dirs...]
"""
import json
import sys
from pathlib import Path


# How each run was actually paid for.  claude-code always reports `total_cost_usd` at public
# API rates, even when the run was served by a Claude subscription and cost nothing marginal.
# Reporting that number as spend would be wrong by a wide margin, so runs are tagged.
BILLING = {
    "core12-opus47": "sub", "core13-opus47": "sub", "full86-opus47": "sub",
    "max-hard-opus47": "sub", "max-full-opus47": "sub", "core13-opus47-r3": "sub",
}
DEFAULT_BILLING = "openrouter"


def _load(p: Path):
    for line in p.read_text(errors="replace").splitlines():
        line = line.strip()
        if line:
            try:
                yield json.loads(line)
            except Exception:
                pass


def claude_code_usage(p: Path) -> dict | None:
    out = None
    for d in _load(p):
        if d.get("type") == "result":
            u = d.get("usage", {}) or {}
            out = {
                "harness": "claude-code",
                "input": u.get("input_tokens", 0),
                "cache_write": u.get("cache_creation_input_tokens", 0),
                "cache_read": u.get("cache_read_input_tokens", 0),
                "output": u.get("output_tokens", 0),
                "reasoning": (u.get("output_tokens_details") or {}).get("thinking_tokens", 0),
                "cost_usd": d.get("total_cost_usd"),
                "turns": d.get("num_turns"),
                "minutes": round(d.get("duration_ms", 0) / 60000, 1),
                "ended": d.get("terminal_reason") or d.get("stop_reason"),
            }
    return out


def codex_usage(p: Path) -> dict | None:
    out = None
    n_items = 0
    for d in _load(p):
        if d.get("type") == "item.completed":
            n_items += 1
        if d.get("type") == "turn.completed":
            u = d.get("usage", {}) or {}
            out = {
                "harness": "codex",
                "input": u.get("input_tokens", 0),
                "cache_write": u.get("cache_write_input_tokens", 0),
                "cache_read": u.get("cached_input_tokens", 0),
                "output": u.get("output_tokens", 0),
                "reasoning": u.get("reasoning_output_tokens", 0),
                "cost_usd": None,          # codex does not report cost
                "turns": None,
                "minutes": None,
                "ended": "turn.completed",
            }
    if out:
        out["turns"] = n_items
    return out


def collect(jobs_dir: Path) -> list[dict]:
    rows = []
    for job in sorted(jobs_dir.iterdir()):
        if not job.is_dir():
            continue
        for trial in sorted(job.glob("*__*")):
            rec = {"job": job.name}
            rj = trial / "verifier" / "reward.json"
            if rj.exists():
                d = json.loads(rj.read_text())
                rec["reward"] = round(d.get("reward", 0), 4)
                for k in ("generative", "tier_core", "tier_gen", "tier_ext", "overclaim_rate"):
                    if k in d:
                        rec[k] = round(d[k], 4) if isinstance(d[k], float) else d[k]
                rec["edges"] = sum(v for k, v in d.items() if k.startswith("edge_"))
            cc, cx = trial / "agent" / "claude-code.txt", trial / "agent" / "codex.txt"
            u = claude_code_usage(cc) if cc.exists() else (codex_usage(cx) if cx.exists() else None)
            if u:
                rec.update(u)
            rec["billing"] = BILLING.get(job.name, DEFAULT_BILLING)
            if len(rec) > 2:
                rows.append(rec)
    return rows


def main():
    dirs = [Path(a) for a in sys.argv[1:]] or [Path("build/jobs")]
    rows = [r for d in dirs if d.exists() for r in collect(d)]
    rows = [r for r in rows if r.get("harness")]        # model runs only
    if not rows:
        print("no model trials found")
        return
    hdr = f"{'run':22s} {'harness':12s} {'reward':>7s} {'edges':>5s} {'in':>9s} {'cache_rd':>9s} {'out':>8s} {'reason':>8s} {'cost':>9s} {'min':>5s}"
    print(hdr); print("-" * len(hdr))
    tot = {"input": 0, "cache_read": 0, "output": 0, "cost": 0.0}
    for r in rows:
        cost = r.get("cost_usd")
        sub = r.get("billing") == "sub"
        cs = ("n/a" if not isinstance(cost, (int, float))
              else (f"~${cost:.2f}*" if sub else f"${cost:.3f}"))
        print(f"{r['job'][:22]:22s} {r['harness']:12s} {r.get('reward','-')!s:>7s} {r.get('edges','-')!s:>5s} "
              f"{r.get('input',0):>9,} {r.get('cache_read',0):>9,} {r.get('output',0):>8,} "
              f"{r.get('reasoning',0):>8,} {cs:>9s} {r.get('minutes','-')!s:>5s}")
        tot["input"] += r.get("input", 0); tot["cache_read"] += r.get("cache_read", 0)
        tot["output"] += r.get("output", 0)
        if isinstance(cost, (int, float)) and not sub:
            tot["cost"] += cost
    print("-" * len(hdr))
    print(f"{'TOTAL':22s} {'':12s} {'':>7s} {'':>5s} {tot['input']:>9,} {tot['cache_read']:>9,} "
          f"{tot['output']:>8,} {'':>8s} ${tot['cost']:.3f}")
    print("\nTOTAL counts only metered spend. '*' = served by a Claude subscription: the harness "
          "still reports a public-rate figure, shown for scale, but nothing was billed per token."
          "\n'n/a' = the harness reports no cost at all (codex).")


if __name__ == "__main__":
    main()
