# Planted edges and generative contract properties — validated, with witnesses

Sixteen S2 slots. **E01–E12 are planted edges**: (a) implemented in `refldx`, (b) absent from
`SPEC.md`, (c) revealed by a short `refldx` invocation on an input constructible from the
visible workspace, and (d) tested by exactly one named test in the S2 suite.

**E13–E16 are no longer planted edges.** The four generative edges were cut — see *"The
generative tier: what was cut and why"* below — and the four slots now hold the generative
**contract properties**, which `SPEC.md` §11 states outright and `tests/genprop.py` checks
without ever comparing the agent's pixels against the reference's.

**These are not guesses.** `build/find_edges.py` encodes, for each decision point, the
*plausible alternative implementation* a competent engineer would write from `SPEC.md` alone,
then searches for the smallest input that separates it from the reference. An edge earns its
place only if a natural alternative measurably diverges. Re-run with:

```
.venv/bin/python build/find_edges.py <path-to-refldx>
```

## Sharpness measurements (2026-09-09, against the shipped binary)

| Edge | Alternative that a careful reader would write | Separating inputs found | Max divergence |
|---|---|---|---|
| E05 | Apply the `SPEC.md` blend table directly, as written for an opaque backdrop | 168 of 168 tested | **254 of 255** |
| E11 | Multiply nested group opacities, round once at the end | 6079 triples | alpha off by 1 at every level, compounding |
| E06 | Apply layer opacity first, then the mask | 1956 triples | 1 per stage, compounding through groups |
| E02 | Assume `merge-down` always preserves appearance | 13 of 16 cases differ *when a non-normal blend sits below* | **29 per channel** |
| E04 | `(acc + half) >> shift` on a negative accumulator | every negative tie | 1 per stage, ×3 stages in `px-unsharp` |
| E03 | Let the accumulator wrap in a `uint8_t` | every overflow | up to 255 |

### E05 is the sharpest and deserves its own note

`SPEC.md` gives the blend formulas "for a fully opaque destination". The natural reading is to
apply that table directly. The reference instead interpolates between the source colour and the
blended colour by *backdrop alpha*:

```
b = ((255 - ad)·cs + ad·blend(cb, cs) + 127) / 255
```

With a nearly transparent backdrop (`ad = 1`) and `multiply`, the textbook reading returns 0
where the reference returns the source colour almost unchanged. Every one of the 168 separating
inputs matched the reference model and none matched the textbook model, with divergence up to
254 of 255. An agent that tests blends only on an opaque background will never see it.

## Edge table

| # | Edge | Reference behaviour | Revealing invocation |
|---|---|---|---|
| E01 | Composite rounding at exactly .5 | round half **up** | `doc-new --fill 0,0,0,1` + `layer-add --fill 1,1,1,1` + `doc-flatten` → colour `01`, truncation gives `00` |
| E02 | `layer-merge-down` composite | composites through the same `composite_pixel` path as flatten, into straight alpha with opacity 255 and blend normal. Appearance **is** preserved in the ordinary case and is **not** preserved when a non-normal blend sits below the merge or the merge happens inside a group with opacity ≠ 255 | merge then flatten vs flatten directly: identical on all 7 stock assets, but differs in 13 of 16 blend×opacity cases once a non-normal blend is below |
| E03 | Channel arithmetic overflow | clamp 0..255, never wrap | `px-offset --arg 100` on 200 → `ff` |
| E04 | Kernel per-stage rounding tie | half **away from zero**, per stage | `px-convolve --kernel 0,0,0,0,-128,0,0,0,0 --shift 8 --bias 128` on 3 → `126`; an arithmetic shift gives `127` |
| E05 | Blend over a partially transparent backdrop | blend is interpolated by backdrop alpha, so a transparent backdrop passes the source through | `doc-new --fill 0,0,0,1` + `layer-add --fill 60,60,60,255 --blend multiply` + flatten → `60`, the table alone gives `0` |
| E06 | Mask vs layer opacity | mask first, then opacity, each rounded | alpha 12, mask 12, opacity 133 → alpha `1`; the other order gives `0` |
| E07 | Out-of-canvas rectangle | clamp, `W_CROP_CLAMPED`, exit 0; empty is `E_BAD_RECT` exit 2 | `px-crop --x -2 --y -2 --w 5 --h 5` |
| E08 | Empty group on flatten / ungroup | records removed, `layer_count` drops | flatten a document holding an empty group |
| E09 | `px-rot270` vs `px-rot90`×3 | bit-identical, and ×4 is the identity | verified on a 5×3 RGB document with alpha and a group |
| E10 | Three-channel op on a gray document | `E_MODE_UNSUPPORTED`, exit 3, no output file | `px-channel-swap` on `assets/blank_gray_16x16.ldx` |
| E11 | Nested group opacity | `round(parent × child / 255)` **at every level** | opacities 1/134/134 → value `1`; rounding once at the end gives `0` |
| E12 | Reserved bytes and record padding | zero-filled; the reader rejects anything else | flip a pad byte, then `doc-open` → `E_BAD_FILE` exit 4 |
| E13 | `gen-scale` seam optimality — *contract, stated in `SPEC.md` §11.5* | the seam removed is connected and carries no more energy than the best seam in the document, plus `2·J·(h−1)` for `--jitter J` | a noisy 16×8 field with one three-column constant corridor: column 7 is the only zero-energy seam and every other column costs ≥ 544, so at `--jitter 0` the bound admits the corridor and nothing else |
| E14 | `gen-fill` provenance — *contract, §11.4* | every synthesised pixel is a copy of a pixel at a legal source position, nothing outside the region moves, and the filled patches match real source patches | invent a colour, or copy one legal colour everywhere, and the check names the first offending pixel |
| E15 | `gen-heal` smoothness — *contract, §11.6* | the relaxed region stays inside the range of values already in the layer and its discontinuity is within 1.5× the reference's | leaving the region untouched scores 3 205 929 against a bound of 506 900; painting it black is rejected by the range test |
| E16 | `gen-*` determinism — *contract, §11.1* | one seed and one input give byte-identical output on a second run, for all four commands, with a structurally conformant file every time | any search that leaks iteration order, an address, the clock or the locale into its result fails on the second run |

### The generative tier: what was cut and why

The four generative commands are search algorithms driven by a seeded integer PRNG. Grading
them by byte comparison forced a dilemma with no honest answer:

- The **first** `SPEC.md` gave them 209 words and called the schedules "discoverable by
  experiment". That was unfair. Reconstructing a PatchMatch cost function, an LCG, a field
  split, a propagation order and a draw order from black-box output is not experiment, it is
  search over an astronomically large space. Opus 4.7 skipped the tier entirely, which was
  the rational response.
- The **second** `SPEC.md` specified the algorithms at implementable detail — the exact LCG
  constants, the field split, the luma weights, the energy formula, the DP recurrence, the
  patch cost, the three phases, the relaxation rule. That made the tier fair and made it
  worthless: it handed over most of the implementation, and the task degraded into
  transcription.

The mistake was in the verifier, not in the prose. **Byte-exact comparison is the wrong
instrument for an output whose exact form cannot be fairly specified.** `SPEC.md` §11 now
describes each command's objective and output in natural language and states the contract the
grader enforces; `tests/genprop.py` enforces it. No generative byte comparison remains
anywhere in the verifier: S1 compares the generative tier with `compare(..., "gen")`, which
checks the exit code, stdout, the stderr codes and the whole structure of the file *except*
its pixels, and S4 and S6 skip the payload for those four operations too.

**The four hidden decisions are cut, all of them.** None survives as a graded edge, because
none is observable once byte-identity is dropped:

| Cut | What it was | Why it cannot be graded |
|---|---|---|
| `gen-scale` tie-break | equal-energy DP paths resolve to the lowest column | two minimum-energy seams are equally good answers; the verifier now computes the minimum itself and accepts any seam that reaches it |
| `gen-fill` schedule | scanline direction alternates per iteration, search radius halves per step | a purely internal schedule with no observable consequence; an exhaustive nearest-neighbour fill with no schedule at all satisfies the objective better |
| `gen-heal` sweep count | exactly 64 relaxations, no convergence exit | nothing in the output reveals an iteration count. 32 sweeps, 64 and 1 000 all satisfy the smoothness bound, and they should: a smoother answer is a better one |
| `gen-*` draw consumption | a rejected candidate has still advanced the stream | invisible unless the PRNG is the reference's, which is exactly what is no longer required |

The slots are reused, not padded. E13 and E14 restate the two behaviours that *do* survive as
observable properties — a seam carver really must find a minimum-energy seam, and a fill
really must copy rather than invent — and E15 and E16 hold the two contract promises that
replace what was cut. All four are stated in `SPEC.md`, so an agent is told exactly what it
must achieve; and since every bound is expressed against what `refldx` achieves on the same
input, and the agent has `refldx`, an agent can measure its own score before it is graded.

### The flat-document problem, and the textured probe

The hidden documents are mostly flat, and **a flat document cannot tell a real search from a
lazy one**: every seam carries the same energy, every patch matches every other patch, and a
region left alone is already smooth. The first run of the property verifier proved it — a
deliberately wrong generative family scored the same generative tier as the reference.

So each generative invocation is graded twice: once on the hidden document, and once on a
*textured probe* derived from it by `genprop.texturize()` — the same size, the same records,
the same metadata, with every raster payload replaced by a 5x3 tile of pseudo-random colours.
Tiling rather than noise is what makes all four objectives meaningful at once: patches repeat,
so a real fill scores near zero and a lazy one does not; the tile edges are sharp, so a region
left alone is measurably rougher than one that was relaxed; and the content genuinely
continues, so a strip that keeps scanning beats one that repeats the edge line.

### Evidence that the tier discriminates

`build/genprop_selftest.py` runs the whole property set offline against a faithful Python port
of `fam_gen.c`: **0 reference failures** over seven canvas shapes, flat, smooth and textured
content and the full parameter grid, and **112 of 136 wrong implementations caught**. Every
miss is `gen-extend` on a canvas with too few lines to carry a continuation, or one where the
wrong strip is byte-identical to a strip a correct implementation would also have produced.
It also pins the two legitimate variations the contract must not punish: 32 relaxation sweeps
instead of 64, and 1 000 instead of 64, both accepted.

End to end through the real harness, on E2B:

| Run | reward | tier_gen | E13 | E14 | E15 | E16 | Core / Ext / S3 / S4 / S5 |
|---|---|---|---|---|---|---|---|
| oracle (`genprop-oracle5`) | **1.000** | 1.00 | pass | pass | pass | pass | all 1.0 |
| `build/wrong_gen_task.py` (`genprop-wrong2`) | 0.901 | **0.00** | **fail** | **fail** | **fail** | pass | all 1.0 |

The wrong family is the reference source with four surgical edits — `gen-fill` paints an
invented colour, `gen-scale` removes the rightmost column, `gen-heal` leaves the region
untouched, `gen-extend` replicates the edge line — so everything outside `fam_gen.c` stays
green and the only thing the difference can be measuring is the generative contract. All four
generative commands fail S1; the named messages were

```
gen-fill    pixel 0,0 was invented: 070707ff is not the value of any legal source position
gen-scale   the removed seam carries energy 131; the best seam carries 96 and the
            allowance at --jitter 0 is 96
gen-heal    region discontinuity 293445 exceeds the bound 102670 (reference 68287)
            (on the textured probe input)
gen-extend  continuation cost 127372 exceeds the bound 109680 (reference 54588)
```

E16 passes for the wrong family, and should: a wrong search can still be a deterministic one.
That is the point of keeping determinism as its own named property rather than folding it
into the others.

### The tier grew to six

`gen-retarget` and `gen-denoise` were added to Tier G after the four above. They carry no
named S2 slot — E13-E16 still name the original four — and are graded entirely through the
Tier G conformance grid, where every case runs the same `genprop` contract check on the hidden
document and on the textured probe, and an operation passes only if every one of its cases
does.

| Command | Contract `genprop` enforces |
|---|---|
| `gen-retarget` (`--seed --h --jitter`) | each column of the output is that column of the input with exactly one pixel removed or spliced in, in lockstep across every pixel-bearing record; the removed or inserted rows form a connected horizontal seam; at `--h` one away from the current height the seam carries no more energy than the best horizontal seam, plus `2*J*(w-1)` |
| `gen-denoise` (`--seed --index --radius --strength`) | only the target record moves and its alpha does not; every colour value stays inside the range already present in that channel of that layer; `--radius 0` and `--strength 0` are the identity; pixels with identical clamped neighbourhoods get identical values (purity); and the layer's adjacent-pair roughness is no larger than the input's and within `1.5x` the reference's |

`gen-retarget` is `gen-scale` reflected through the diagonal, and both sides of the task share
their machinery rather than duplicating it: `fam_gen.c` has one `gen_energy()` and one
`gen_seam_dp()` that both commands enter with the axes swapped, and `genprop._transpose()`
feeds the horizontal case into the very same seam dynamic program the vertical case uses. A
divergence between the two orientations is therefore not expressible.

`gen-denoise` is the one member of the family whose search is exhaustive rather than sampled,
so it consumes no draws and does not depend on the seed's value -- which `SPEC.md` 11.1 allows
outright. Determinism is still a real property for it, and the half of it a single output file
can show is checked: a window filter is a function of the neighbourhood its window sees and of
nothing else, so two pixels whose clamped surroundings and edge clipping match must be given
the same value.

`build/wrong_gen_task.py` now makes six surgical edits rather than four -- `gen-retarget`
removes the bottom row instead of a seam, `gen-denoise` computes its weighted average and then
hands back the input pixel:

| Run | reward | tier_gen | gen_property_rate | Core / Ext / S3 / S4 / S5 |
|---|---|---|---|---|
| oracle (`retarget-oracle`) | **1.000** | 1.00 (6/6) | 1.00 | all 1.0 |
| six-way wrong family (`genprop-wrong-6`) | 0.777 | **0.00** (0/6) | 0.25 | all 1.0 |

```
gen-retarget  the removed seam carries energy 907; the best seam carries 852 and the
              allowance at --jitter 0 is 852 (on the textured probe input)
gen-denoise   the denoised layer carries roughness 12745986, above the bound 1163564
              the reference's 771501 allows: the layer was not denoised
              (on the textured probe input)
```

A denoiser that invents colours instead of blending real ones is caught even harder: against
the same 40 hidden documents it is rejected on every one of them by the provenance envelope,
because that check is exact and does not need the content to be textured.

**One pre-existing bug fell out of the new validation sweep and was fixed.** `gen-heal`'s
Gauss-Seidel sweep counts its in-canvas 4-neighbours and divides by that count; on a 1x1
canvas the count is zero, so the reference divided by zero — undefined behaviour, which the
toolchain happened to render as 0 and which therefore violated `gen-heal`'s own range promise
(§11.6) on four of the forty hidden documents. `fam_gen.c` now leaves a pixel with no
neighbours alone, which is the only answer a membrane with no Dirichlet data can give. Without
it the oracle's Tier G result was a coin flip whenever S1's three-document sample happened to
draw a 1x1 canvas for `gen-heal`.

## Invariants the reference does satisfy

Worth stating because an agent can use them as self-checks, and because breaking them is a
fast way to detect a wrong implementation:

- `px-rot90` applied four times is the identity, and `px-rot270` equals `px-rot90`×3, byte for
  byte, including on documents with masks and nested groups.
- `px-scale --num 1 --den 1`, `px-blur-box --radius 0`, `px-sharpen --amount 0` and
  `px-dither --levels 256` are all byte-exact no-ops.
- `doc-save` is a byte-identical round trip on any well-formed document.
- gray → rgb → gray is the identity; rgb → gray → rgb is not.

## Invariants the reference deliberately does NOT satisfy

- `layer-merge-down` followed by `doc-flatten` equals flattening directly **only** while the
  merge sits over a `normal`-blended backdrop at group opacity 255. Introduce a non-normal
  blend below the merge, or perform it inside a group whose opacity is not 255, and the two
  diverge by up to 29 per channel, because source-over is not associative under blend modes
  and the baked layer opacity is rounded a second time against the group's.

  This is the sharpest structure of all the edges: the invariant *holds* on every stock asset
  and on the obvious test, so an agent verifies it once, concludes merge-down is associative,
  and is wrong exactly where it matters. That is E02.
- Convolution samples out-of-canvas taps by **clamp to edge**, not zero padding. On a 1×1
  canvas a nine-tap averaging kernel therefore saturates to 255 rather than returning the
  input.
