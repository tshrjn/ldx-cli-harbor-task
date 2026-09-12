#!/usr/bin/env python3
"""Materialise build/wrong-gen-task/: the real task, with a deliberately wrong generative
family installed as the agent deliverable.

A property verifier that passes everything is worthless, so this is the discrimination test
for `tests/genprop.py`.  Everything outside `fam_gen.c` is the reference source, so Core,
Extended, E01-E12, the fuzz and the scenarios all stay green and the only thing the run can
be measuring is the generative contract.

The six commands keep the reference's argument parsing, its error codes and its container
writing, and break exactly one promise each:

    gen-fill      paints the region a colour it invented   -> provenance
    gen-scale     removes the rightmost column, not a seam -> seam optimality
    gen-heal      leaves the region exactly as it found it -> smoothness
    gen-extend    replicates the edge line forever         -> continuation cost
    gen-retarget  removes the bottom row, not a seam       -> seam optimality
    gen-denoise   hands back the input unchanged           -> smoothness

    .venv/bin/python build/wrong_gen_task.py
    harbor run -p ./collinear-candidate/ldx-cli/build/wrong-gen-task -a oracle -e e2b \\
        -o ./collinear-candidate/ldx-cli/build/jobs --job-name genprop-wrong -y
"""
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TASK = HERE.parent
DEST = HERE / "wrong-gen-task"

SOLVE = """#!/bin/bash
# Deliberately wrong generative family, installed as the agent deliverable.
set -euo pipefail
mkdir -p /app/src
cp /solution/*.c /app/src/
cp /solution/Makefile /app/src/Makefile
make -C /app/src
cp /solution/conformance.json /app/conformance.json
echo "wrong-gen: installed"
"""

# (marker, replacement) applied to solution/fam_gen.c, each asserted to hit exactly once.
PATCHES = [
    # gen-fill: invent a colour instead of copying one.
    ("""    /* RECONSTRUCT */
    for (p = 0; p < npx; p++) {
        size_t s;
        int c;
        if (!reg[p]) continue;
        s = (size_t)nny[p] * w + (size_t)nnx[p];
        for (c = 0; c < f.ch; c++) L->data[p * (size_t)f.ch + (size_t)c] = L->data[s * (size_t)f.ch + (size_t)c];
    }""",
     """    /* WRONG-GEN: paint the hole a colour of our own invention. */
    for (p = 0; p < npx; p++) {
        int c;
        if (!reg[p]) continue;
        for (c = 0; c < f.ch; c++)
            L->data[p * (size_t)f.ch + (size_t)c] = (uint8_t)(c == f.ch - 1 ? 255 : 7);
    }"""),
    # gen-scale: take the rightmost column instead of a minimum-energy seam.
    ("""    while (d->w > (uint32_t)target) {
        gen_seam(d, &rng, jitter, seam);
        gen_seam_apply(d, seam, 0);
    }
    while (d->w < (uint32_t)target) {
        gen_seam(d, &rng, jitter, seam);
        gen_seam_apply(d, seam, 1);
    }""",
     """    /* WRONG-GEN: always the rightmost column, never a seam. */
    while (d->w != (uint32_t)target) {
        uint32_t yy;
        int grow = d->w < (uint32_t)target;
        gen_seam(d, &rng, jitter, seam);
        for (yy = 0; yy < d->h; yy++) seam[yy] = d->w - 1;
        gen_seam_apply(d, seam, grow);
    }"""),
    # gen-heal: do nothing at all to the region.
    ("""    /* E15: exactly GEN_HEAL_ITERS sweeps, unconditionally */
    for (it = 0; it < GEN_HEAL_ITERS; it++)""",
     """    /* WRONG-GEN: leave the region exactly as we found it. */
    for (it = 0; it < 0; it++)"""),
    ("""            for (probe = 0; probe < GEN_PROBES; probe++) {
                long dx, dy, sx, sy;
                size_t s;
                int c;
                gen_offset(gen_draw(&rng), radius, &dx, &dy); /* E16 */""",
     """            for (probe = 0; probe < 0; probe++) {
                long dx, dy, sx, sy;
                size_t s;
                int c;
                gen_offset(gen_draw(&rng), radius, &dx, &dy); /* E16 */"""),
    # gen-extend: replicate the edge line for the whole strip.
    ("""        src[j] = (int32_t)best;
        prev = best;""",
     """        /* WRONG-GEN: never move off the edge line. */
        src[j] = (int32_t)(step > 0 ? lines - 1 : 0);
        (void)best;"""),
    # gen-retarget: take the bottom row instead of a minimum-energy horizontal seam.
    ("""    while (d->h > (uint32_t)target) {
        gen_hseam(d, &rng, jitter, seam);
        gen_hseam_apply(d, seam, 0);
    }
    while (d->h < (uint32_t)target) {
        gen_hseam(d, &rng, jitter, seam);
        gen_hseam_apply(d, seam, 1);
    }""",
     """    /* WRONG-GEN: always the bottom row, never a seam. */
    while (d->h != (uint32_t)target) {
        uint32_t xx;
        int grow = d->h < (uint32_t)target;
        gen_hseam(d, &rng, jitter, seam);
        for (xx = 0; xx < d->w; xx++) seam[xx] = d->h - 1;
        gen_hseam_apply(d, seam, grow);
    }"""),
    # gen-denoise: compute the weighted average, then hand back the input pixel.
    ("""            for (c = 0; c < cc; c++)
                L->data[((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c] =
                    (uint8_t)div_round(acc[c], wsum);""",
     """            /* WRONG-GEN: throw the average away and keep the input pixel. */
            (void)acc;
            (void)wsum;
            for (c = 0; c < cc; c++)
                L->data[((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c] =
                    src[((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c];"""),
]


def main():
    src = (TASK / "solution" / "fam_gen.c").read_text()
    for marker, repl in PATCHES:
        if src.count(marker) != 1:
            print(f"patch marker not found exactly once:\n{marker[:90]}", file=sys.stderr)
            return 1
        src = src.replace(marker, repl)

    if DEST.exists():
        shutil.rmtree(DEST)
    (DEST / "solution").mkdir(parents=True)
    for item in ("instruction.md", "task.toml"):
        shutil.copy(TASK / item, DEST / item)
    shutil.copytree(TASK / "environment", DEST / "environment")
    shutil.copytree(TASK / "tests", DEST / "tests", ignore=shutil.ignore_patterns("__pycache__"))
    (DEST / "tests" / "refldx").chmod(0o700)
    for item in TASK.glob("solution/*.c"):
        shutil.copy(item, DEST / "solution" / item.name)
    shutil.copy(TASK / "solution" / "Makefile", DEST / "solution" / "Makefile")
    shutil.copy(TASK / "solution" / "conformance.json", DEST / "solution" / "conformance.json")
    (DEST / "solution" / "fam_gen.c").write_text(src)
    (DEST / "solution" / "solve.sh").write_text(SOLVE)
    (DEST / "solution" / "solve.sh").chmod(0o755)
    print(f"wrote {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
