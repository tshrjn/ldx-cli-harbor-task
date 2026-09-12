#!/usr/bin/env python3
"""Materialise build/core-task/: the Core tier alone, for fast probes.

The full task grades 91 commands across three tiers and takes hours. This slice grades only
Tier C — the 13 Core command names and the 12 planted edges — so a model's ability to do the
deep semantic work can be measured without committing to the whole surface.

Because the Generative and Extended tiers do not exist here, their reward components are
removed and the remainder renormalised, rather than scoring them zero and calling a Core-only
submission a failure.

    .venv/bin/python build/core_task.py
    harbor run -p ./collinear-candidate/ldx-cli/build/core-task -a claude-code -m <model> -e e2b
"""
import re
import shutil
from pathlib import Path

TASK = Path(__file__).resolve().parent.parent
CORE_OPS = ["doc-new", "doc-open", "doc-save", "layer-add", "layer-set", "layer-reorder",
            "layer-merge-down", "layer-mask-apply", "doc-flatten", "px-crop", "px-transform",
            "px-channel", "px-convolve"]
DEST = TASK / "build" / "core-task"

INSTRUCTION_SCOPE = '''`refldx` implements 91 commands. **Only the 13 Core command names are graded here:**
`doc-new`, `doc-open`, `doc-save`, `layer-add`, `layer-set`, `layer-reorder`,
`layer-merge-down`, `layer-mask-apply`, `doc-flatten`, `px-crop`, `px-transform`,
`px-channel`, `px-convolve`. The rest exist and you may explore them freely, but they are not
scored and `compare.sh` will show rows for them.

This tier carries all twelve planted edge behaviours, so it is the deep end of the task, not
a warm-up. Scoring is linear with partial credit — there is no minimum.

'''


def patch_opspec(src: str) -> str:
    """Grade Core only: the other two tiers become empty."""
    src = re.sub(r"^TIER_GEN = \[[^\]]*\]", "TIER_GEN = []  # core-only slice", src, count=1, flags=re.M | re.S)
    src = re.sub(r"^TIER_EXT = \[[^\]]*\]", "TIER_EXT = []  # core-only slice", src, count=1, flags=re.M | re.S)
    # The generator table still covers all 91 commands; ALL_OPS is now a subset of it, which
    # is exactly what a slice means, so relax the equality self-check to containment.
    src = src.replace('assert set(_GEN) == set(ALL_OPS), "generator table and ALL_OPS disagree"',
                      'assert set(ALL_OPS) <= set(_GEN), "ALL_OPS has an op the generator table lacks"')
    return src


def patch_verify(src: str) -> str:
    """Drop the generative component and renormalise; empty tiers must not score zero."""
    src = src.replace(
        "    core_rate = core_ops / n_core if n_core else 0.0\n"
        "    gen_rate = gen_ops / n_gen if n_gen else 0.0\n"
        "    ext_rate = ext_ops / n_ext if n_ext else 0.0",
        "    core_rate = core_ops / n_core if n_core else 1.0\n"
        "    gen_rate = gen_ops / n_gen if n_gen else 1.0    # empty tier: nothing to fail\n"
        "    ext_rate = ext_ops / n_ext if n_ext else 1.0")
    src = src.replace(
        "        overall = (0.25 * generative + 0.25 * functional + 0.25 * constraint\n"
        "                   + 0.15 * robustness + 0.10 * artifact)",
        "        # Core-only slice: no generative tier exists, so its 0.25 is redistributed\n"
        "        # across the components that do apply rather than scored as a failure.\n"
        "        overall = (0.35 * functional + 0.35 * constraint\n"
        "                   + 0.20 * robustness + 0.10 * artifact)")
    src = src.replace('s1_core_ext = (0.40 * core_r + 0.35 * ext_r) / 0.75', 's1_core_ext = core_r')
    return src


def main():
    if DEST.exists():
        shutil.rmtree(DEST)
    DEST.mkdir(parents=True)
    for item in ("task.toml", "README.md"):
        shutil.copy(TASK / item, DEST / item)
    for d in ("environment", "tests", "solution"):
        shutil.copytree(TASK / d, DEST / d)
    (DEST / "tests" / "refldx").chmod(0o700)
    (DEST / "solution" / "solve.sh").chmod(0o755)
    for p in DEST.rglob("__pycache__"):
        shutil.rmtree(p, ignore_errors=True)

    instr = (TASK / "instruction.md").read_text()
    start = instr.index("`refldx` implements")
    end = instr.index("Deliverables:")
    (DEST / "instruction.md").write_text(instr[:start] + INSTRUCTION_SCOPE + instr[end:])

    # The attestation must cover exactly the ops this slice grades, in both the blank template
    # the agent copies and the oracle's filled-in version.
    import json
    import sys
    sys.path.insert(0, str(DEST / "tests"))
    core = json.loads((TASK / "environment/workspace/conformance.template.json").read_text())
    keep = [o for o in core["ops"] if o in CORE_OPS]
    for f, filled in ((DEST / "environment/workspace/conformance.template.json", False),
                      (DEST / "solution/conformance.json", True)):
        d = json.loads(f.read_text())
        d["ops"] = {o: ({"implemented": True, "verified": True, "notes": ""} if filled
                        else {"implemented": False, "verified": False, "notes": ""}) for o in keep}
        d.pop("tiers", None)
        f.write_text(json.dumps(d, indent=1) + "\n")

    (DEST / "tests" / "opspec.py").write_text(patch_opspec((TASK / "tests" / "opspec.py").read_text()))
    (DEST / "tests" / "verify.py").write_text(patch_verify((TASK / "tests" / "verify.py").read_text()))
    print(f"wrote {DEST} — Core tier only, 13 command names, 12 edges")


if __name__ == "__main__":
    main()
