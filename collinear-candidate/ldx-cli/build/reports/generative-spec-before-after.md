# Generative tier — the specification, before and after

Three states exist. V1 shipped, V2 was written and rejected, V3 ships now.

| | words | how it graded | verdict |
|---|---|---|---|
| **V1** original | 209 | byte-exact against `refldx` | unimplementable in practice — see below |
| **V2** my first fix | ~1,400 | byte-exact against `refldx` | fair but it handed over the implementation |
| **V3** current | 1797 | property contract, no byte comparison | ships |

## Why V1 failed, precisely

Each command got **one table row**. To match bytes an implementer had to reconstruct the
pseudo-random generator, its field split, the patch cost function, the phase order, the
propagation direction, the radius schedule and the draw-consumption rule — roughly eight
unknowns, each with many values. Byte-equality then returns **one bit for the conjunction**:
an implementation with seven of eight right scores exactly the same as an empty file. That is
not experimentation, and the measured consequence was that Claude Opus 4.7 skipped the entire
tier.

## Why V2 was rejected

V2 fixed fairness by writing the algorithms out: the LCG constants `1664525 / 1013904223`, the
`m = draw ^ (draw >> 15)` field split, the patch cost, the three phases, the DP recurrence,
the relaxation rule. It was implementable — and it reduced the task to transcription. Fair and
worthless are not far apart when the fix is "say more".

## The V3 move

Stop specifying the search; specify the **contract**, and grade against that. Two
implementations that both satisfy it are both correct even where they disagree pixel for
pixel. The objective function is stated — you cannot fairly grade someone against a bound you
refuse to name — and nothing else is.

Verified absent from V3: `1664525`, `1013904223`, `0x7fff`, "PatchMatch", "Gauss-Seidel", the
DP recurrence. Present, because the grader computes against them: the luma weights and the
energy definition.

---

# V1 — exact text as shipped

```markdown
## 10. Generative operations

Four commands are *algorithmically* generative: their output is the result of a search rather
than a direct function of the input. Each takes a required `--seed <u32>` and is fully
deterministic given one — the same seed on the same input always produces byte-identical
output. No network, no model, no floating point.

| Command | Flags | What it does |
|---|---|---|
| `gen-fill` | `--seed --index --radius --iters --x --y --w --h` | fills a region by searching the rest of the layer for matching patches |
| `gen-scale` | `--seed --w --jitter` | changes width by removing or inserting seams chosen from a gradient energy map |
| `gen-heal` | `--seed --index --radius --x --y --w --h` | reconstructs a region by relaxation against its boundary |
| `gen-extend` | `--seed --dir --amount --radius` | grows the canvas and synthesises the new strip by continuing existing content |

The region for `gen-fill` and `gen-heal` is the layer's mask record, or the rectangle given by
`--x --y --w --h`.

This document does **not** state the iteration schedules, the propagation order, the
tie-breaking rules, or the order in which each algorithm consumes its random stream. Those are
defined by `refldx` and are discoverable by experiment.

```

---

# V3 — exact text as it ships now

```markdown
## 11. Generative operations

`gen-fill`, `gen-scale`, `gen-heal` and `gen-extend` are *algorithmically* generative: each
output is the result of a search rather than a closed-form function of the input. This
section tells you what each command must **achieve** and what its output must **look like**.
It deliberately does not tell you how to search, and no amount of running `refldx` is meant
to make you reconstruct its search: **these four commands are not graded by comparing your
bytes against `refldx`'s bytes.** They are graded against the contract below. Two
implementations that both satisfy it are both correct, even where they disagree pixel for
pixel.

Everything else in this document still applies to them without exception: the container
layout, the record metadata, the zero-filled padding, the structured error codes and the exit
codes are graded exactly as they are for every other command.

### 11.1 What all four promise

1. **Determinism.** The same input, the same flags and the same `--seed` produce
   byte-identical output on every run. Nothing may reach the output from the clock, an
   address, the locale, uninitialised memory, or iteration over an unordered container.
   `--seed` is required and takes 0..2147483647; it exists so that a randomised search can be
   reproduced. It is *not* required that two seeds differ in their output — an implementation
   that meets the objective without randomness is correct, and simply ignores the seed.
2. **Integers only.** No floating point anywhere. Averages round half up.
3. **Structure survives.** The output re-parses under the strict reader; the canvas changes
   exactly as the flags demand and in no other way; record count, order, kind, name, opacity,
   blend, flags and parent are carried through unchanged; padding and reserved bytes stay
   zero-filled; and every record that carries pixels — masks included — receives the same
   geometry as the raster records, so a mask stays registered with its layer.
4. **Region discipline.** Every pixel the command was not asked to change survives unchanged.
   For `gen-fill` and `gen-heal` that is everything outside the region, in every record; for
   `gen-extend` it is the whole of the original canvas; for `gen-scale` it is every pixel of
   every row that the seams did not take.
5. **Provenance.** Synthesised content comes from content that was already in the document.
   These commands move, copy and blend pixels; they do not invent colours. `gen-fill`,
   `gen-scale` and `gen-extend` copy outright; the two blends — `gen-scale`'s inserted pixel
   (§11.5) and `gen-heal`'s interpolation (§11.6) — are averages of pixels that were there,
   and each is bounded in its own section.

### 11.2 The region (`gen-fill`, `gen-heal`)

The region to synthesise comes from one of two places, in this order:

1. `--x --y --w --h`, an explicit rectangle; all four must be given together, or it is
   `E_BAD_ARGS`, exit 2. An out-of-canvas rectangle follows the same rule as `px-crop`.
2. Otherwise the mask record belonging to the target layer. A mask byte is in the region when
   it is **non-zero**; the threshold is 1, not 128.

A layer with neither is `E_BAD_ARGS`, exit 2. `--index` must name a raster layer.

### 11.3 Alpha

`gen-fill` and `gen-heal` synthesise missing content, so they write **every** channel of the
target record, alpha included: a hole has no alpha any more than it has colour. `gen-scale`
and `gen-extend` move whole pixels, so alpha rides along untouched.

### 11.4 `gen-fill` — fill a region from elsewhere in the layer

```
gen-fill --seed S [--index I] [--radius R] [--iters N] [--x X --y Y --w W --h H]
```
defaults: index 0, radius 2, iters 4.

**Objective.** Replace the region with content that looks like the rest of the layer:
around every filled pixel, the neighbourhood you produce should match some neighbourhood that
really occurs in the surviving content.

**Output.** Every pixel of the region, in every channel, is a copy of a pixel taken from a
**legal source position**: one whose whole (2R+1)×(2R+1) patch lies inside the canvas and
contains no region pixel. A layer with no legal source position at all is `E_BAD_ARGS`, exit
2. `--iters` bounds the effort your search spends; it does not change what counts as correct.

**Judged on.** Region discipline and provenance, both exactly. Then two measures of whether
the fill is really a fill:

- *variation*, the squared difference across adjacent pixel pairs inside the region. Yours
  must be at least a quarter of what `refldx` achieves. Stamping one legal colour over the
  whole region satisfies provenance and is still not a fill; this is what says so. Where the
  surviving content is flat, so is `refldx`'s fill, and the floor does not apply.
- *patch coherence*, the squared difference between the (2R+1)×(2R+1) patch your output puts
  at each filled pixel and the closest patch at any legal source position, summed over the
  region and every channel. Yours must not exceed three times `refldx`'s. This is a ceiling on
  incoherence, not a demand for optimality: deep inside a large region nothing constrains the
  answer, and the grader does not pretend otherwise.

Running `refldx` on your own test inputs tells you both numbers you are measured against.

### 11.5 `gen-scale` — content-aware width change

```
gen-scale --seed S --w W [--jitter J]
```
jitter default 0.

**Objective.** Reach the target width by removing (or inserting) the columns that matter
least, one **seam** at a time. A seam is a connected path with exactly one pixel per row,
moving at most one column between one row and the next. `--w` below the current width removes
seams, above it inserts them, equal to it is a rewrite; the height never changes.

**Energy.** This is the measure the grader uses, so it is written out in full. Composite the
document exactly as `doc-flatten` would, reduce it to integer luma

```
Y = (77*r + 150*g + 29*b + 128) >> 8            (a gray document uses its one channel)
```

and take the L1 gradient magnitude with clamp-to-edge central differences:

```
E(x, y) = |Y(x+1, y) - Y(x-1, y)| + |Y(x, y+1) - Y(x, y-1)|
```

The cost of a seam is the sum of `E` over the pixels it passes through. One energy map drives
every record, so layers and masks lose the same column in the same row and stay in lockstep.

**Output.** After removing one seam, each row of each record is that row of the input with
exactly one pixel deleted, and the deleted columns form a connected seam. After inserting
one, each row is that row of the input with one pixel spliced in, and the spliced pixel is the
round-half-up average of the two pixels it was spliced between. Removing or inserting several
seams composes these one at a time.

**Judged on.** The structure above, exactly; then, when `--w` differs from the current width
by one, the energy of the seam you took, which must not exceed the energy of the best seam in
the document by more than `2 * J * (height - 1)`. With the default `--jitter 0` that means
the seam must be a minimum-energy seam: the grader computes the minimum itself. `--jitter J`
buys exactly that much slack, and is there so a seeded search can trade optimality for
variety. Several seams at once are judged the same way with a looser bound, because the
energy map is remade after each seam.

### 11.6 `gen-heal` — smooth a region into its surroundings

```
gen-heal --seed S [--index I] [--radius R] [--x X --y Y --w W --h H]
```
defaults: index 0, radius 8.

**Objective.** Replace the region with a smooth interpolation of the content around it, so
that the region and its boundary read as one surface rather than as a patch. `--radius`
bounds how far your search may reach for the content it starts from.

**Output.** Every channel of every region pixel is written; nothing outside the region is
touched. No value may fall outside the range of values already present in that channel of the
target layer — the result is a blend of content that was there, never a new colour.

**Judged on.** Region discipline and that range, both exactly; then *discontinuity* — the
squared difference across every horizontally or vertically adjacent pair of pixels with at
least one endpoint in the region, summed over every channel. Yours must not exceed one and a
half times what `refldx` achieves on the same input plus a per-adjacency allowance. A smoother
answer than `refldx`'s is never penalised, so how far you take the interpolation is up to you.

### 11.7 `gen-extend` — grow the canvas by continuing the content

```
gen-extend --seed S --dir left|right|top|bottom --amount N [--radius R]
```
radius default 16.

**Objective.** Grow the canvas by `N` in `--dir` and fill the new strip with more of the same
content, laid down one line at a time — columns for `left`/`right`, rows for `top`/`bottom`.
`--radius` bounds how far your search may reach for a source line.

**Output.** The original content is copied verbatim into the new canvas, offset by `N` when
the growth is on the `left` or `top`. Every new line is a verbatim copy of one whole existing
line of the input, and every pixel-bearing record copies the **same** source line, so layers
and masks stay registered. A source line is legal only when the line before it, in the
direction of growth, is also on the canvas; when the source has fewer than two lines there is
no legal choice and the edge line is replicated.

**Judged on.** That structure, exactly; then *continuation cost* — for each new line, the
squared difference between the line you laid it against and the line that precedes your chosen
source line in the direction of growth, summed over the composited colour channels and over
the whole strip. This is the measure of "the content really does continue". Yours must not
exceed twice what `refldx` achieves on the same input plus a per-sample allowance.

### 11.8 What this section does not tell you, and why

It does not tell you how any of the four searches works: no pseudo-random recipe, no
propagation order, no iteration counts, no internal cost function beyond the four measures
above, which are the grading criteria and nothing else. That is deliberate. The exact output
of a seeded search cannot be specified in prose without handing over the implementation, so
it is not specified, and it is not graded. What is graded is what a correct implementation
owes its user: a deterministic result, a well-formed file, discipline about what it touched,
honesty about where its pixels came from, and a demonstrable answer to the objective.

Every bound above is stated against what `refldx` achieves on the same input, and you have
`refldx`. Measure it, measure yourself, and you know your score before you are graded.

```
