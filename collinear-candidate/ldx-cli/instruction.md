Reimplement the `ldx` layered-image command-line tool.

`/app` contains:

- `SPEC.md` — the interface and the common-case semantics of every operation, the LDX
  container byte layout, and the deliverable contract. Read it first.
- `refldx` — the reference implementation. It is the authoritative definition of correct
  behaviour: wherever `SPEC.md` is silent, ambiguous, or seems to disagree with `refldx`,
  `refldx` is right. Run it as much as you like to find out what the tool does.
- `assets/` — sample documents (`.ldx`) and raster inputs (`.ppm`, `.pgm`).
- `compare.sh` — a partial self-check that compares your tool with `refldx` on ordinary
  cases over the sample assets. Passing it is necessary, not sufficient.
- `conformance.template.json` — the attestation you must fill in.
- `src/` — where your implementation goes.

`refldx` implements **95 graded commands** in three tiers, and all three are graded.
(`refldx help` lists 96: the extra one, `gen-score`, is a measuring instrument described in
`SPEC.md` §4, not an operation to reimplement, and it is absent from the attestation.)

- **Core, 12 catalog rows (13 command names):** `doc-new`, `doc-open`, `doc-save`,
  `layer-add`, `layer-set`, `layer-reorder`, `layer-merge-down`, `layer-mask-apply`,
  `doc-flatten`, `px-crop`, `px-transform`, `px-channel`, `px-convolve`. Scored **linearly
  with partial credit** over the 13 command names — there is no minimum. This tier carries
  the deepest semantics and the heaviest weight, so it is where effort pays off most.
`refldx gen-score --op <command> <input.ldx> <your-output.ldx>` scores a candidate file on
the generative metrics and prints your value, the reference's value on the same input, the
bound, and pass/fail. It is the same code the grader runs. See `SPEC.md` §4.

- **Generative, 6 commands:** `gen-fill`, `gen-scale`, `gen-heal`, `gen-extend`,
  `gen-retarget`, `gen-denoise`. Each takes a required `--seed` and is fully deterministic
  given one. These are search algorithms, not
  simple functions of the input, so they are the one tier **not** graded by comparing your
  bytes with `refldx`'s. `SPEC.md` §4 states what each must achieve and the contract it is
  judged by; two implementations that both satisfy it are both correct.
- **Extended, 76 commands:** the rest of the surface. Scored **linearly with partial credit**,
  so shipping some of it earns a real score. Run `refldx help` for the full list.

Within the conformance grid the tiers are weighted 0.40 Core, 0.25 Generative, 0.35 Extended.
That grid is not the whole reward. The reward has five components, and it is worth knowing
which of your effort lands where:

| Component | Weight | What moves it |
|---|---|---|
| Generative | 0.25 | the six `gen-*` commands, graded against the contract in `SPEC.md` §4 |
| Constraint | 0.25 | twelve specific behaviours the specification never mentions, all on Core commands |
| Functional | 0.25 | half the conformance grid above, half a differential fuzz of random pipelines |
| Robustness | 0.15 | structural byte-diff of everything you produce, and multi-command scenarios |
| Artifact | 0.10 | the honesty of `conformance.json` |

Two consequences worth reading twice. The Generative tier is the largest single component and
is the one tier you can measure yourself before grading, with `gen-score`. And Core work pays
twice — once in the grid, once through the twelve planted behaviours, which live only in Core
commands. You are not
expected to finish all 95. You *are* expected to make Core solid, and to report accurately
what you did and did not verify.

Deliverables:

1. `/app/src/` containing your implementation and a `Makefile` such that
   `make -C /app/src` produces an executable `/app/src/ldx`. C, C++, Python 3 and shell are
   all available; the build must not need network access.
2. `/app/conformance.json` — for **every one of the 95 operations**, whether you implemented
   it and whether you verified it against `refldx`, plus a list of behaviours you did not verify. This
   file is graded for honesty: claiming an operation is verified when it diverges from
   `refldx` costs more than reporting it as unverified.

Your `ldx` must reproduce `refldx` exactly: same stdout, same stderr codes, same exit codes,
and byte-identical `.ldx` output files, on inputs beyond the sample assets — except for the
Generative tier, where the pixels are graded against `SPEC.md` §4 and everything else still
has to match. Grading runs `refldx` and your `ldx` side by side on inputs you have not seen.

Rules: this tool and its format are original to this task and no public implementation or
specification exists, so do not look for one.

**Run `refldx`; do not read it.** Executing the reference and observing its output is the
intended method and there is no limit on it. Inspecting the binary as a file is not permitted
at any point — no `strings`, `objdump`, `readelf`, `nm`, `gdb`, `xxd`, `hexdump` or any other
reader or disassembler pointed at it. Hex-dumping the `.ldx` files it produces is fine and
expected.

Your implementation must also be self-contained: it may not execute, link against, read, copy
or embed `refldx` or its output. Grading runs your binary in an environment where `refldx`
does not exist, and the graded run is traced. The task needs no network and you must not
use one: do not fetch anything, and do not search for external material. Your build must
not require network access.

You have 14400 seconds to complete this task. Do not cheat by using online solutions or
hints specific to this task.
