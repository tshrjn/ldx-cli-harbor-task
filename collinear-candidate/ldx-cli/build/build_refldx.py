#!/usr/bin/env python3
"""Build the reference binary as a static, stripped linux/amd64 ELF in an E2B sandbox.

Usage:  .venv/bin/python build/build_refldx.py        (needs E2B_API_KEY)

Static linking means the binary does not depend on the build image's libc, so the default
E2B template (Debian 12) produces a binary that runs unchanged in the task image
(ubuntu:24.04).  Writes environment/workspace/refldx (agent copy, 0755) and tests/refldx
(verifier oracle copy, 0700), syncs the source into solution/ldx.c, and records the sha256.
"""
import hashlib
import shutil
import sys
from pathlib import Path

from e2b import Sandbox

HERE = Path(__file__).resolve().parent
TASK = HERE.parent
SRC = HERE / "refldx-src" / "ldx.c"

# Hardening note.  The rule is "run the reference, do not read it", and the gates in
# tests/integrity.py catch a deliverable that embeds or executes refldx.  Neither stops an
# agent decompiling a static binary and porting it, so the build also removes what makes
# that cheap: -s drops the symbol table, --build-id=none drops the fingerprint, and the
# unwind-table flags drop .eh_frame, which is what a decompiler leans on to recover function
# boundaries once symbols are gone.  This raises the cost; it does not make it impossible,
# and the honest control remains the trajectory audit (build/audit_traces.py), which sees
# how many times the agent actually ran the reference.
#
# Every flag below was verified behaviour-neutral: 28 invocations spanning the geometry,
# filter and generative families across three seeds produce byte-identical output, stdout,
# stderr and exit codes under the hardened and unhardened builds.
CC = ("cd /tmp && gcc -O2 -std=c99 -Wall -Wextra -static -s -fno-tree-vectorize "
      "-fno-asynchronous-unwind-tables -fno-unwind-tables -fomit-frame-pointer "
      "-ffunction-sections -fdata-sections "
      "-Wl,--gc-sections -Wl,--build-id=none -o refldx ldx.c")


def main() -> int:
    sb = Sandbox.create(timeout=600)
    try:
        for f in sorted(SRC.parent.glob("*.c")):
            sb.files.write(f"/tmp/{f.name}", f.read_text())
        r = sb.commands.run(f"{CC} && ls -l refldx && file refldx && sha256sum refldx", timeout=300)
        print(r.stdout)
        if r.exit_code != 0:
            print(r.stderr, file=sys.stderr)
            return 1
        blob = sb.files.read("/tmp/refldx", format="bytes")
    finally:
        sb.kill()

    digest = hashlib.sha256(blob).hexdigest()
    for dest, mode in ((TASK / "environment/workspace/refldx", 0o755), (TASK / "tests/refldx", 0o700)):
        dest.write_bytes(blob)
        dest.chmod(mode)
        print(f"wrote {dest.relative_to(TASK)} ({len(blob)} bytes, mode {oct(mode)})")
    for f in sorted(SRC.parent.glob("*.c")):
        shutil.copy(f, TASK / "solution" / f.name)
    (HERE / "refldx.sha256").write_text(digest + "\n")
    print("sha256", digest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
