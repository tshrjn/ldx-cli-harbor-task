#!/usr/bin/env python3
"""Materialise build/naive-task/: a copy of the task whose solution/ is the deliberately
shallow implementation in build/naive/.

This lets the naive discrimination dry run go through the *real* harness — the oracle agent
installs build/naive/ instead of the reference source — rather than a bespoke replay:

    .venv/bin/python build/naive_task.py
    harbor run -p ./collinear-candidate/ldx-cli/build/naive-task -a oracle -e e2b

The environment/ directory is copied verbatim, so E2B reuses the same template as the real
task.  build/naive-task/ is regenerated on demand and is not part of the shipped task.
"""
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
TASK = HERE.parent
import sys
VARIANT = sys.argv[1] if len(sys.argv) > 1 else "naive"
DEST = HERE / f"{VARIANT}-task"

SOLVE = """#!/bin/bash
# Naive dry run: installs the shallow implementation as the agent deliverable.
set -euo pipefail
mkdir -p /app/src
cp /solution/ldx.py /app/src/ldx.py
cp /solution/Makefile /app/src/Makefile
make -C /app/src
cp /solution/conformance.json /app/conformance.json
echo "naive: installed"
"""


def main():
    if DEST.exists():
        shutil.rmtree(DEST)
    (DEST / "solution").mkdir(parents=True)
    for item in ("instruction.md", "task.toml"):
        shutil.copy(TASK / item, DEST / item)
    shutil.copytree(TASK / "environment", DEST / "environment")
    shutil.copytree(TASK / "tests", DEST / "tests")
    (DEST / "tests" / "refldx").chmod(0o700)
    for item in ("ldx.py", "Makefile", "conformance.json"):
        shutil.copy(HERE / VARIANT / item, DEST / "solution" / item)
    (DEST / "solution" / "solve.sh").write_text(SOLVE)
    (DEST / "solution" / "solve.sh").chmod(0o755)
    print(f"wrote {DEST}")


if __name__ == "__main__":
    main()
