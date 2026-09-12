# Is the SPEC v2 cut fair? — measured, not asserted

SPEC v2 removes ~400 of v1's 556 lines. The claim that justifies it is narrow and testable:

> Every fact removed is obtainable by running `refldx`, which the agent has.

This is that test. Each row was run against a build of the shipped reference
(`build/refldx-src/ldx.c`); the probe is `build/spec_v2_probe.sh`.

| v1 section | Lines cut | Recovered by | Verdict |
|---|---:|---|---|
| §2 exit codes / error codes | 15 | one bad invocation per code | **printed verbatim** |
| §2 value ranges | 10 | boundary bisection — but see below | **printed verbatim** |
| §3 document model | 28 | `doc-open --dump` names every field | recovered |
| §4 per-op semantics | 73 | run the op, diff in/out | recovered |
| §5 rendering + blend table | 25 | one `doc-flatten` per blend mode | **recovered exactly** |
| §6 container byte layout | 34 | `xxd` on any output file | recovered |
| §7 `doc-open` JSON | 14 | run `doc-open` | **printed verbatim** |
| §8 PNM input | 6 | public format | n/a |

## The finding that matters

The ranges table was not merely discoverable — **the reference prints it**:

```
$ ldx doc-new --w 0 --h 4 --mode rgb a.ldx
{"code":"E_BAD_ARGS","message":"--w out of range (1..16384)"}
```

`message` is free text by contract, so this is not something the task promised. It is
something the reference happens to do, and it means v1 §2's ranges table was redundant twice
over. Same for §7: `doc-open` emits its own schema on any document.

## Blend math: the cleanest case for cutting

v1 §5 printed the formula for all six blend modes. One flatten per mode recovers every one of
them, against a backdrop of `200,60,255` and a source of `100,150,50` at alpha 255:

| blend | output | v1's stated formula, evaluated |
|---|---|---|
| `normal` | `649632` | `cs` = 100,150,50 ✓ |
| `multiply` | `4e2332` | `round(cb·cs/255)` = 78,35,50 ✓ |
| `screen` | `deafff` | `cb+cs−round(cb·cs/255)` = 222,175,255 ✓ |
| `darken` | `643c32` | `min` = 100,60,50 ✓ |
| `lighten` | `c896ff` | `max` = 200,150,255 ✓ |
| `difference`| `645acd` | `|cb−cs|` = 100,90,205 ✓ |

Six invocations replace 25 lines of specification, and the agent that runs them learns the
rounding direction as well — which v1 stated only as "rounded to the nearest integer" and
never disambiguated at exactly .5. That disambiguation is planted edge E01.

## What the probe does NOT establish

Three things, stated so the reduction is not oversold:

1. **Discoverable ≠ cheap.** Recovering §4's 73 lines means running every operation and
   diffing outputs. That is the intended cost — it converts reading into experiment — but it
   consumes budget that v1 handed over free. Expect scores to fall on the same models; that is
   the point, and it is only fair if the budget is adequate. Untested.
2. **§11 is not covered here.** The generative tier's metrics are the one thing genuinely not
   recoverable from `refldx`'s I/O, which is why v2 replaces the prose with a `gen-score`
   subcommand rather than simply deleting it. Until `gen-score` exists, cutting §11 would make
   the tier unfair, not merely harder.
3. **The oracle has not been re-run against v2.** The v1 fairness argument rested on the
   oracle scoring 1.0 and the naive implementations not improving. That matrix has to be
   re-run before v2 ships. Nothing here substitutes for it.
