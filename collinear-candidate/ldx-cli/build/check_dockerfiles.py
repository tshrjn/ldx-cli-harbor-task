#!/usr/bin/env python3
"""Fail if the agent image and the clean-room verifier image could drift apart.

A deliverable that compiles in one and not the other would charge a harness fault to the
agent, so the base digest and the package line must be identical in both.
"""
import re
import sys
from pathlib import Path

TASK = Path(__file__).resolve().parent.parent


def key_lines(p: Path):
    t = p.read_text()
    frm = re.search(r"^FROM (.*)$", t, re.M)
    run = re.search(r"^RUN apt-get update.*?rm -rf /var/lib/apt/lists/\*$", t, re.M | re.S)
    if not frm or not run:
        sys.exit(f"{p}: no FROM or apt line found")
    return frm.group(1).strip(), " ".join(run.group(0).split())


def main() -> int:
    a = key_lines(TASK / "environment/Dockerfile")
    b = key_lines(TASK / "tests/Dockerfile")
    if a[0] != b[0]:
        print(f"BASE IMAGE DRIFT\n  environment: {a[0]}\n  tests:       {b[0]}")
        return 1
    if a[1] != b[1]:
        print(f"PACKAGE DRIFT\n  environment: {a[1]}\n  tests:       {b[1]}")
        return 1
    print(f"images agree: {a[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
