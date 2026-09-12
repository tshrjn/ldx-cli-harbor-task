# Catalog v2 — the composite binary grows a hard tier

## Why these and not more of the same

v1 bought its horizon with breadth: 91 commands, each shallow enough to read off `refldx` in a
few invocations. Opus 4.7 scored 0.846 on Core and 0.403 on Extended, which says the breadth
is doing its job as a triage problem and almost nothing as a *difficulty* problem — the
Extended shortfall is unfinished work, not failed work.

v2 adds difficulty where discovery is expensive: families whose behaviour is fully visible in
the bytes `refldx` writes, and still takes real experimentation to pin down. Every family
below is deterministic, integer-closed, offline, and byte-checkable. None of them can be
guessed from the operation's name, and none of them needs a word of specification — which is
what lets SPEC v2 drop to 151 lines while the task gets harder.

The test each family had to pass: **an implementer who knows the domain still cannot write it
correctly without running the reference.**

## X1. Container codecs — 5 commands

| Command | Flags |
|---|---|
| `doc-save` (extended) | `--compress none\|rle\|packbits\|delta` |
| `doc-recompress` | `--compress ... [--index I]` |
| `doc-verify` | `--strict` — reports codec//padding/checksum faults without rewriting |
| `doc-stat-size` | per-record compressed and raw sizes |
| `doc-repack` | re-lay records without re-encoding |

Record payloads gain a per-record codec byte. Byte-exact output requires reproducing the
encoder's *decisions*, not merely a decoder: where a run becomes cheaper than literals, the
minimum run length, whether runs may cross a row boundary, how the final short run is emitted,
what happens to a run of exactly 2, and which codec the encoder picks when two tie on size.

This is the sharpest family in the task. Every one of those decisions is invisible to a reader
of the format and obvious in a hexdump of the output — the ideal ratio.

*Grading:* byte comparison. *Edge candidates:* E17 run-of-2, E18 row-boundary reset, E19
tie-break between codecs at equal size, E20 trailing-literal flush.

## X2. Indexed colour and palettes — 6 commands

| Command | Flags |
|---|---|
| `doc-convert` (extended) | `--mode indexed --colours N` |
| `pal-build` | `--colours N --seed S` |
| `pal-remap` | `--from doc.ldx` — re-index onto another document's palette |
| `pal-sort` | `--by luma\|population\|rgb` |
| `pal-merge` | `--tolerance T` |
| `px-dither-pal` | `--method none\|ordered\|error-diffuse` |

`color_mode 2` exists in the header and v1 rejects it with `E_UNSUPPORTED`. v2 implements it:
a palette record kind, index payloads, and a quantiser.

Quantisation is a search, so it splits the way the generative tier does — **the container side
is byte-exact** (palette record layout, index payload, ordering, padding) and **the quantiser
is contract-graded** (every index in range; the palette covers the image within a bounded
error against what `refldx` achieves; determinism under a seed; provenance — no colour in the
palette that was not in the image). `pal-sort` and `pal-remap` are pure functions and stay
byte-exact.

*Grading:* mixed, byte-exact + `gen-score --op pal-build`. *Edges:* E21 index tie at equal
distance, E22 palette shorter than `--colours` when the image has fewer distinct colours.

## X3. Vector paths — 5 commands

| Command | Flags |
|---|---|
| `path-add` | `--points x,y,... [--closed 0\|1] [--parent P]` |
| `path-fill` | `--index I --rule even-odd\|nonzero --colour C` |
| `path-stroke` | `--index I --width W --join miter\|bevel\|round --cap butt\|square\|round` |
| `path-transform` | `--op ... ` — keeps paths registered with raster records |
| `path-to-mask` | `--index I --rule ...` |

A fifth record kind carrying a polyline. Rasterisation is integer scanline fill, and the
difficulty is entirely in the tie-breaking: which side of a pixel centre counts as inside, what
happens when a vertex lands exactly on a scanline, how a horizontal edge is treated, whether
the right and bottom edges are inclusive, and how a stroke's miter behaves past the miter
limit. Every one of those is a half-pixel decision that changes bytes and cannot be reasoned
out from first principles, because more than one convention is defensible.

*Grading:* byte comparison. *Edges:* E23 vertex on scanline, E24 horizontal edge in even-odd,
E25 miter limit fallback to bevel.

## X4. Tiled storage — 3 commands

| Command | Flags |
|---|---|
| `doc-tile` | `--size 8\|16\|32\|64 [--dedup 0\|1]` |
| `doc-untile` | |
| `tile-stat` | unique/total tile counts per record |

Payloads stored as fixed tiles, with identical tiles stored once and referenced. Byte-exactness
needs the exact tile traversal order, the dedup key, the reference encoding, and how a canvas
that is not a whole number of tiles pads its last row and column.

*Grading:* byte comparison. *Edges:* E26 partial edge tile padding, E27 dedup reference to a
tile that appears later in traversal order.

## X5. Script execution — 1 command

| Command | Flags |
|---|---|
| `doc-script` | `--file ops.txt [--dry-run]` |

Executes a file of `ldx` invocations against one document in memory. The work is not the
evaluator, it is the failure semantics: the exact `E_*` code and message shape for a
malformed line, the 1-based line number, whether a failure at line N leaves the output file
absent or partial, and whether `--dry-run` validates every line or stops at the first fault.

*Grading:* byte comparison, including stderr. *Edge:* E28 transactional rollback — a failure
at the last line must leave no output file at all.

## X6. Extended blend modes — 0 new commands

`overlay` `hard-light` `soft-light` `color-dodge` `color-burn` `exclusion` join the six v1
modes, everywhere a blend is accepted. No new commands, but every compositing path in the
task gets six more integer-rounding behaviours to discover, and `doc-flatten`,
`layer-merge-down` and the seam energy all inherit them.

*Grading:* byte comparison. *Edges:* E29 `color-dodge` division by a zero backdrop, E30
`soft-light` rounding at the 128 hinge.

## Totals

| | v1 | v2 |
|---|---|---|
| Commands | 91 | **111** |
| Tiers | Core 13 / Gen 6 / Ext 72 | Core 13 / Gen 7 / Ext 72 / **Composite 19** |
| Planted edges | 16 | **30** |
| SPEC.md | 556 lines | **151 lines** |

`gen-score` grows one op (`pal-build`), which is why Generative reads 7.

Suggested weights, keeping Core dominant and making the new tier worth real points:
`0.30 core + 0.20 generative + 0.20 extended + 0.30 composite` inside S1.

## Build cost, honestly

`refldx` is 6,283 lines of C across seven family files. X1/X4/X6 are cheap — codecs and blend
modes are self-contained and the record plumbing already exists. X2 and X3 are not: indexed
mode touches every operation that assumes 1 or 3 channels, and paths add a record kind that
every geometry operation must then keep registered. X5 is cheap in code and expensive in
edge-case design.

Order to build, by value per hour: **X1 → X6 → X4 → X5 → X3 → X2.** X1 alone, at four planted
edges and pure byte-exactness, is worth more difficulty than the whole Extended tier.
