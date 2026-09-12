# `tests/opspec.py` — interface the verifier depends on

The verifier generates its whole conformance grid from this module, so 86 operations do not
mean 86 hand-written tests. Nothing here may import anything outside the standard library.

```python
TIER_CORE: list[str]   # exactly the 12 Tier C rows (doc-open and doc-save both listed)
TIER_GEN:  list[str]   # the 4 gen-* commands
TIER_EXT:  list[str]   # every remaining command
ALL_OPS:   list[str]   # TIER_CORE + TIER_GEN + TIER_EXT, matching `refldx help` exactly

def tier_of(op: str) -> str      # "core" | "gen" | "ext"

def cases(op: str, rng: random.Random, ctx: dict) -> list[tuple[list, dict]]
    """Return canonical invocations for one operation.

    Each element is (argv, extra_inputs):
      argv          list of strings/ints, WITHOUT the binary name, using the literal
                    placeholders "IN.ldx" for the input document and "OUT.ldx" for the
                    output document. Commands that print instead of writing a file simply
                    omit "OUT.ldx".
      extra_inputs  {relative_name: absolute_path} for any additional file the command
                    needs (a PNM for --from, a second document for doc-diff, ...). The
                    verifier copies these next to IN.ldx before running.

    ctx describes the document that will be supplied as IN.ldx:
      {"w": int, "h": int, "mode": "rgb"|"gray", "alpha": 0|1,
       "channels": int, "pnm": str, "alpha_pnm": str, "file": str,
       "layers": [ {"index": int, "kind": str, "parent": int}, ... ]}

    Return between 2 and 6 cases per op: at least one ordinary case, and where the grammar
    allows it one boundary case (extreme but VALID parameters) and one error case (a
    deliberately invalid parameter, to check the exit code and error code match).
    Never return a case whose arguments are invalid for this ctx — e.g. do not emit an
    rgb-only flag when ctx["mode"] == "gray"; pick a different case instead.
    """
```

Rules that keep the grid meaningful:

- Draw every parameter from `rng` so the grid differs run to run, but stay inside the valid
  range declared by the command's own `allowed[]`/`parse_int` bounds in the family source.
- Layer indices must exist in `ctx["layers"]` and be of the right kind for the command.
- `gen-*` commands must always pass `--seed`, drawn from `rng`.
- Commands that print to stdout write no file; the verifier compares stdout instead.
