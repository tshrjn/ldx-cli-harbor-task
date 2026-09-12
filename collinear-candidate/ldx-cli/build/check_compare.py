#!/usr/bin/env python3
"""Run the agent-facing compare.sh in an E2B sandbox with the oracle source and with the
naive source, to confirm the self-check correlates with the verifier score.

Usage: .venv/bin/python build/check_compare.py [naive naive-plus ...]   (needs E2B_API_KEY)
"""
import sys
from pathlib import Path

from e2b import Sandbox
from e2b.sandbox.filesystem.filesystem import WriteEntry

TASK = Path(__file__).resolve().parent.parent


def upload(sb: Sandbox, local: Path, remote: str, names=None):
    entries = [WriteEntry(path=str(Path(remote) / f.relative_to(local)), data=f.read_bytes())
               for f in sorted(local.rglob("*")) if f.is_file() and (names is None or f.name in names)]
    for i in range(0, len(entries), 20):
        sb.files.write_files(entries[i:i + 20])


def main():
    sb = Sandbox.create(timeout=900)
    try:
        upload(sb, TASK / "environment" / "workspace", "/app")
        sb.commands.run("chmod 0755 /app/refldx /app/compare.sh")
        variants = [("oracle", TASK / "solution", {"ldx.c", "Makefile"})]
        variants += [(v, TASK / "build" / v, {"ldx.py", "Makefile"}) for v in (sys.argv[1:] or ["naive"])]
        for label, src, names in variants:
            sb.commands.run("rm -rf /app/src && mkdir -p /app/src")
            upload(sb, src, "/app/src", names)
            r = sb.commands.run("cd /app && ./compare.sh", timeout=600, request_timeout=600)
            print(f"== compare.sh with {label} source (rc={r.exit_code})\n{r.stdout}{r.stderr[-400:]}")
    finally:
        sb.kill()


if __name__ == "__main__":
    main()
