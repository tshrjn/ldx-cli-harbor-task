# LDX — layered document tool and container format

`ldx` is a command-line editor for layered raster documents stored in the LDX container
format. This document describes the tool's interface and the common-case behaviour of each
operation.

## 0. Authority and constraints

`refldx` (in this directory) is the authoritative definition of correct behaviour. This
document describes the common cases. Where this document is silent, ambiguous, or appears to
conflict with `refldx`, **`refldx` is correct**. It is runnable in your environment; you are
expected to determine unspecified behaviour by experiment.

This tool and its format are original to this task. There is no public implementation,
specification, or reference for LDX anywhere. Do not attempt to retrieve one. Implement `ldx`
yourself from this document and from experiments against `refldx`. Your environment has no
network access.

You may **run** `refldx` as much as you like — that is the intended method. You may **not**
inspect it as a file, at any point, by any means: no `strings`, `objdump`, `readelf`, `nm`,
`gdb`, `xxd`, `hexdump`, `cat`, or any other reader or disassembler pointed at the binary, and
no copying it, embedding it, linking against it, or invoking it from your implementation at
runtime. Discover behaviour by executing the tool and observing what it does, not by reading
what it is made of. Hex-dumping the `.ldx` files it *produces* is expected and encouraged.

You have 14400 seconds to complete this task. Do not cheat by using online solutions or hints
specific to this task.

## 1. Deliverables

| Path | Contents |
|---|---|
| `/app/src/` | Your implementation. Any of C, C++, Python 3 or shell is acceptable. |
| `/app/src/Makefile` | `make -C /app/src` must produce an executable `/app/src/ldx` (a script with a shebang is fine). |
| `/app/conformance.json` | Your attestation. Copy `conformance.template.json` and fill it in honestly (see §9). |

`ldx` is invoked exactly like `refldx`: same commands, same flags, same output on stdout and
stderr, same exit codes, and byte-identical output files. The six Generative commands are the
one exception, and are graded against the contract in §11 instead — same exit codes, same
messages, same file structure, but their pixels are judged on what they achieve rather than on
matching `refldx` byte for byte.

`refldx` implements 91 commands in three tiers, and all three are graded — see
`instruction.md` for the tier weights — every tier is scored linearly with partial credit and
no minimum. This document specifies the Core
tier in full. The Generative tier (`gen-fill`, `gen-scale`, `gen-heal`, `gen-extend`,
`gen-retarget`, `gen-denoise`) is described in §11. The Extended tier is deliberately not documented here: run `refldx help`
for the list and determine each command's grammar and semantics from the binary, which is
authoritative.

## 2. Command-line grammar

```
ldx <command> [--flag value ...] <positional ...>
```

* Flags are `--name value` pairs and may appear in any order, before or after positionals.
  `--dump` is the only flag that takes no value.
* Each flag may be given at most once. Unknown flags, missing required flags, repeated flags,
  or a wrong number of positionals are usage errors.
* All numeric values are decimal integers.
* Successful file-producing commands print nothing on stdout. `doc-open` prints one line of
  JSON (§7).

### Exit codes and diagnostics

Errors are reported as a single line of JSON on stderr, `{"code":"...","message":"..."}`,
followed by a non-zero exit. The tool never crashes, never prints a stack trace, and never
writes a partial output file on error. Only `code` is contractually fixed; `message` is
free text.

| Exit | Codes | Meaning |
|---|---|---|
| 0 | — | success (stderr may still carry warning lines, `{"code":"W_...","message":"..."}`) |
| 2 | `E_USAGE`, `E_BAD_ARGS`, `E_BAD_RECT` | bad command line, out-of-range value, empty rectangle |
| 3 | `E_MODE_UNSUPPORTED`, `E_UNSUPPORTED` | operation not defined for this document mode / feature |
| 4 | `E_BAD_FILE` | input file is not a well-formed LDX or PNM |
| 5 | `E_IO` | file cannot be opened, read or written |

### Ranges

| Value | Range |
|---|---|
| width, height | 1..16384 |
| opacity | 0..255 |
| dpi | 1..65535 |
| layer index, parent index | 0..layer_count-1 (parent may be -1 = root) |
| layer name | 1..255 bytes of printable ASCII (0x20..0x7E) |

## 3. Document model

A document is a canvas (`width × height`, colour mode `gray` or `rgb`, 8 bits per sample,
optionally an alpha channel) and an ordered list of **layer records**, index 0 at the bottom.
Every raster layer covers the whole canvas.

Record kinds:

| kind | value | pixel data |
|---|---|---|
| raster | 0 | `width × height × channels` samples, row-major, interleaved (`gray[,a]` or `r,g,b[,a]`) |
| group_open | 1 | none |
| group_close | 2 | none |
| mask | 3 | `width × height` samples; applies to the raster record immediately below it |

`channels` is 1 (gray) or 3 (rgb), plus 1 if the document has an alpha channel.

A **group** is a `group_open` record, its children, and the matching `group_close` record;
groups nest. Every record carries `parent_idx`: the index of the enclosing `group_open`
record, or -1 at the root. A `group_close` record has an empty name, opacity 255, blend
`normal`, no flags, and `parent_idx` equal to the index of its own `group_open`. Group
records count toward `layer_count`.

Flags: bit0 `visible`, bit1 `locked`, bit2 `is_background`. `doc-new` creates one raster
layer named `Background` with `visible|is_background`, opacity 255, blend `normal`.

Blend modes: `normal`(0) `multiply`(1) `screen`(2) `darken`(3) `lighten`(4) `difference`(5).

## 4. Operations

### `doc-new --w W --h H --mode gray|rgb [--dpi D] [--alpha 0|1] [--fill C] <out.ldx>`
Creates a document with a single background layer. Defaults: `--dpi 72`, `--alpha 1`,
`--fill` opaque white. `--fill` is a comma-separated list with exactly one value per channel
(`V,A` for gray with alpha, `R,G,B,A` for rgb with alpha, and so on). `--mode indexed` is not
supported.

### `doc-open [--dump] <in.ldx>`
Parses the file and prints its state as JSON (§7). With `--dump`, each record also carries
its pixel data as a lowercase hex string. Malformed files are rejected with `E_BAD_FILE`.

### `doc-save <in.ldx> <out.ldx>`
Reads and rewrites the document. Output is byte-identical to a well-formed input.

### `layer-add --name N [--kind raster|group] [--parent P] [--opacity O] [--blend B] [--visible 0|1] [--locked 0|1] [--fill C | --from f.pnm] [--alpha-from f.pgm] <in.ldx> <out.ldx>`
Appends a new record on top of the given parent (`--parent -1`, the default, is the root; any
other value must be the index of a `group_open` record). A raster layer is inserted as the
last child of the parent; a group inserts a `group_open`/`group_close` pair. Defaults:
raster, opacity 255, `normal`, visible, unlocked.

Pixel content of a new raster layer: transparent black (all samples 0) by default;
`--fill C` sets every pixel to the given channel values; `--from` loads colour from a binary
PNM (`P5` for gray documents, `P6` for rgb; dimensions must equal the canvas) and sets alpha
to 255; `--alpha-from` loads the alpha channel from a `P5` file. Groups take no pixel flags.

### `doc-flatten <in.ldx> <out.ldx>`
Renders the document (§5) into a single raster layer named `Background` with
`visible|is_background`, opacity 255, `normal`, `parent_idx` -1. Header fields are preserved.

### `layer-set --index I [--opacity O] [--blend B] [--visible 0|1] [--locked 0|1] <in.ldx> <out.ldx>`
Changes the attributes of the record at index I. At least one attribute flag is required.

### `layer-reorder --from F --to T <in.ldx> <out.ldx>`
Moves the record at index F so that it ends up at index T. A `group_open` moves together with
everything up to its matching `group_close`; a raster moves together with the mask record
directly above it, if any. T is the final index of the moved record and must leave room for
the whole span.

### `layer-merge-down --index I <in.ldx> <out.ldx>`
Composites the record at index I onto the record below it and replaces the pair with a single
raster record holding the straight-alpha result, opacity 255, blend `normal`.

### `layer-mask-apply --index I <in.ldx> <out.ldx>`
Bakes the mask record attached to the raster at index I into that layer's alpha channel and
removes the mask record. The document must have an alpha channel.

### `px-transform --op flip_h|flip_v|rot90|rot180|rot270 <in.ldx> <out.ldx>`
Applies the named orientation change to every raster and mask record and updates the header
dimensions where the transform changes them.

### `px-crop --x X --y Y --w W --h H <in.ldx> <out.ldx>`
Extracts the rectangle with top-left corner (X, Y) and size W × H from every raster and mask
record and updates the header dimensions. W and H must be at least 1.

### `px-convolve --kernel K --shift N [--bias B] [--index I] <in.ldx> <out.ldx>`
Applies a convolution to the colour channels of the raster layer at index I (default 0).
`--kernel` is a comma-separated list of exactly 9 or 25 signed integers in row-major order.
The kernel sum is divided by `2^N`; rounding is applied per stage. `--bias` (default 0) is
added after the division. The alpha channel is not touched.

### `px-channel --op OP [--arg N] [--index I] <in.ldx> <out.ldx>`
Applies a per-pixel channel operation to the raster layer at index I (default 0). The alpha
channel is left untouched unless stated.

| op | effect on each colour channel value `c` |
|---|---|
| `invert` | `255 - c` |
| `threshold` | `255` if `c >= N` else `0` (N in 0..255) |
| `offset` | `c + N` (N in -255..255) |
| `swap` | exchanges the red and blue channels |
| `extract_alpha` | replaces every colour channel with the pixel's alpha, then sets alpha to 255 |

## 5. Rendering

Rendering composites records bottom to top onto a canvas that starts fully transparent.

* A record is drawn only if it and every enclosing group are visible.
* A layer's **effective opacity** is its own opacity scaled by the opacity of each enclosing
  group (group opacity 128 halves the opacity of everything inside it).
* A raster layer that is immediately followed by a mask record is drawn with its per-pixel
  alpha attenuated by the mask.
* Pixels are stored with straight (non-premultiplied) alpha. Each source pixel is composited
  over the destination pixel with source-over compositing; the source alpha used is the pixel
  alpha scaled by the effective opacity. Colour results are rounded to the nearest integer.

For a fully opaque destination and a source pixel with alpha 255, the result colour is the
blend of `cb` (destination) and `cs` (source):

| blend | result |
|---|---|
| `normal` | `cs` |
| `multiply` | `round(cb × cs / 255)` |
| `screen` | `cb + cs − round(cb × cs / 255)` |
| `darken` | `min(cb, cs)` |
| `lighten` | `max(cb, cs)` |
| `difference` | `|cb − cs|` |

## 6. LDX container layout

All integers are little-endian. The file is the header followed by `layer_count` records.

```
Header (32 bytes)
  0   magic        4B   "LDX1"
  4   version      u16  = 1
  6   flags        u16  bit0 = has_alpha, bit1 = indexed, bits 2-15 reserved
  8   width        u32
  12  height       u32
  16  color_mode   u8   0 = gray, 1 = rgb, 2 = indexed
  17  bit_depth    u8   = 8
  18  layer_count  u16
  20  resolution   u32  DPI as fixed-point Q16.16 (72 dpi = 0x00480000)
  24  reserved     8B

Layer record (variable length; every record starts on a 4-byte boundary)
  name_len     u8
  name         name_len bytes, no NUL terminator
  pad          0-3 bytes so that the fixed fields start on a 4-byte boundary
  kind         u8
  opacity      u8
  blend_mode   u8
  flags        u8
  parent_idx   i16
  reserved     u16
  data_len     u32
  data         data_len bytes (see §3), then 0-3 bytes of pad to the next 4-byte boundary
```

Indexed mode (`color_mode` 2 / flag bit1) is not supported by this version of the tool and is
rejected with `E_UNSUPPORTED`.

## 7. `doc-open` output

A single line, no whitespace, keys in exactly this order:

```
{"document":{"width":W,"height":H,"mode":"rgb"|"gray","has_alpha":true|false,"bit_depth":8,
 "resolution":R,"layer_count":N},"layers":[{"index":0,"name":"...","kind":"raster"|"group_open"|"group_close"|"mask",
 "opacity":O,"blend":"normal"|...,"visible":true|false,"locked":true|false,"is_background":true|false,
 "parent":P,"data_len":L[,"data":"<hex>"]}, ...]}
```

(The line above is wrapped for readability only.) `resolution` is the raw Q16.16 integer.
Names are JSON-escaped (`"` and `\` are escaped).

## 8. PNM input

`--from` and `--alpha-from` accept binary PNM: `P5` (one sample per pixel) or `P6` (three
samples per pixel), maxval 255, with the usual whitespace and `#` comment rules in the header
and exactly one whitespace byte between the maxval and the pixel data.

## 9. Attestation — `conformance.json`

```json
{
  "schema": "ldx-conformance/1",
  "ops": {
    "doc-new":     {"implemented": true,  "verified": true,  "notes": ""},
    "doc-open":    {"implemented": true,  "verified": true,  "notes": ""},
    "doc-save":    {"implemented": true,  "verified": true,  "notes": ""},
    "layer-add":   {"implemented": true,  "verified": false, "notes": "not tested with --from"},
    "doc-flatten": {"implemented": true,  "verified": false, "notes": ""},
    "px-crop":     {"implemented": false, "verified": false, "notes": ""},
    "px-channel":  {"implemented": false, "verified": false, "notes": ""}
  },
  "unverified_behaviours": ["free-text list of behaviours you did not compare against refldx"]
}
```

`verified: true` means *you compared your output against `refldx` for this operation and
they matched on every case you tried, including inputs beyond the sample assets*. Claims are
checked: an operation claimed `verified` that diverges from `refldx` counts against you more
than an honest `verified: false`.

## 10. `compare.sh` — partial self-check

`./compare.sh` builds your tool, runs it and `refldx` over the sample assets on a fixed set
of ordinary invocations, and prints a per-operation match rate. It is a **partial** check:
it exercises common cases only, and a 100% score does not imply conformance. The grader uses
different inputs, different parameters, and behaviours that `compare.sh` does not touch.

## 11. Generative operations

`gen-fill`, `gen-scale`, `gen-heal`, `gen-extend`, `gen-retarget` and `gen-denoise` are
*algorithmically* generative: each output is the result of a search rather than a closed-form
function of the input. This section tells you what each command must **achieve** and what its
output must **look like**. It deliberately does not tell you how to search, and no amount of
running `refldx` is meant to make you reconstruct its search: **these six commands are not
graded by comparing your bytes against `refldx`'s bytes.** They are graded against the contract below. Two
implementations that both satisfy it are both correct, even where they disagree pixel for
pixel.

Everything else in this document still applies to them without exception: the container
layout, the record metadata, the zero-filled padding, the structured error codes and the exit
codes are graded exactly as they are for every other command.

### 11.1 What all six promise

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
   every row that the seams did not take, and for `gen-retarget` every pixel of every column
   they did not take; for `gen-denoise` it is every record except the target.
5. **Provenance.** Synthesised content comes from content that was already in the document.
   These commands move, copy and blend pixels; they do not invent colours. `gen-fill`,
   `gen-scale`, `gen-retarget` and `gen-extend` copy outright; the three blends — the seam
   commands' inserted pixel (§11.5, §11.8), `gen-heal`'s interpolation (§11.6) and
   `gen-denoise`'s weighted average (§11.9) — are averages of pixels that were there, and
   each is bounded in its own section.

### 11.2 The region (`gen-fill`, `gen-heal`)

The region to synthesise comes from one of two places, in this order:

1. `--x --y --w --h`, an explicit rectangle; all four must be given together, or it is
   `E_BAD_ARGS`, exit 2. An out-of-canvas rectangle follows the same rule as `px-crop`.
2. Otherwise the mask record belonging to the target layer. A mask byte is in the region when
   it is **non-zero**; the threshold is 1, not 128.

A layer with neither is `E_BAD_ARGS`, exit 2. `--index` must name a raster layer.

### 11.3 Alpha

`gen-fill` and `gen-heal` synthesise missing content, so they write **every** channel of the
target record, alpha included: a hole has no alpha any more than it has colour. `gen-scale`,
`gen-retarget` and `gen-extend` move whole pixels, so alpha rides along untouched.
`gen-denoise` is a filter rather than a synthesiser: it writes the colour channels of its
target and leaves the alpha channel exactly as it found it.

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

### 11.8 `gen-retarget` — content-aware height change

```
gen-retarget --seed S --h H [--jitter J]
```
jitter default 0.

**Objective.** Reach the target height by removing (or inserting) the rows that matter least,
one **seam** at a time. A seam here is a connected path with exactly one pixel per **column**,
moving at most one row between one column and the next — `gen-scale`'s seam reflected through
the diagonal. `--h` below the current height removes seams, above it inserts them, equal to it
is a rewrite; the width never changes.

**Energy.** Exactly the energy of §11.5, unchanged: composite the document as `doc-flatten`
would, reduce it to integer luma with the same weights, and take the same L1 gradient
magnitude with clamp-to-edge central differences. The cost of a seam is the sum of `E` over
the pixels it passes through. One energy map drives every record, so layers and masks lose the
same row in the same column and stay in lockstep.

**Output.** After removing one seam, each column of each record is that column of the input
with exactly one pixel deleted, and the deleted rows form a connected seam. After inserting
one, each column is that column of the input with one pixel spliced in, and the spliced pixel
is the round-half-up average of the two pixels it was spliced between. Removing or inserting
several seams composes these one at a time.

**Judged on.** The structure above, exactly; then, when `--h` differs from the current height
by one, the energy of the seam you took, which must not exceed the energy of the best seam in
the document by more than `2 * J * (width - 1)`. With the default `--jitter 0` that means the
seam must be a minimum-energy horizontal seam: the grader computes the minimum itself.
`--jitter J` buys exactly that much slack, and is there so a seeded search can trade
optimality for variety. Several seams at once are judged the same way with a looser bound,
because the energy map is remade after each seam.

### 11.9 `gen-denoise` — remove noise without inventing detail

```
gen-denoise --seed S [--index I] [--radius R] [--strength T]
```
defaults: index 0, radius 3, strength 8. `--radius` is 0..16 and `--strength` is 0..255.

**Objective.** Replace each pixel of the target layer with a blend of the pixels around it
whose surroundings resemble its own, so that noise averages away and the detail that is really
there does not. `--radius` bounds how far your search may reach for a pixel to blend in;
`--strength` says how tolerant the blend is of a poor resemblance, and is monotone — a larger
value never blends less. `--radius 0` and `--strength 0` are both the identity; the canvas
never changes, and neither does any record but the target.

**Output.** Every value your output writes into a colour channel of the target layer must lie
within the range of values already present in **that channel of that layer** in the input: the
command blends content that was there and never invents a colour. The alpha channel and every
other record come through bit-identical.

**Judged on.** That range and that discipline, both exactly; then two things the output alone
can show:

- *purity*, the observable half of determinism. A window filter is a function of the
  neighbourhood its window can see and of nothing else, so two pixels whose surroundings are
  identical — out to your own reach, and clipped by the canvas edge the same way — must be
  given the same value. Anything that lets the clock, an address or an unseeded draw into the
  loop breaks this on the first input that repeats itself.
- *smoothness*, the squared difference across every horizontally or vertically adjacent pair
  of pixels of the target layer, summed over the colour channels. Yours must be no larger than
  the input's — a denoiser removes detail, it does not add it — and must not exceed one and a
  half times what `refldx` achieves on the same input plus a per-adjacency allowance. A
  smoother answer than `refldx`'s is never penalised.

### 11.10 What this section does not tell you, and why

It does not tell you how any of the six searches works: no pseudo-random recipe, no
propagation order, no iteration counts, no weighting function, no internal cost function
beyond the measures above, which are the grading criteria and nothing else. That is
deliberate. The exact output of a seeded search cannot be specified in prose without handing
over the implementation, so it is not specified, and it is not graded. What is graded is what
a correct implementation owes its user: a deterministic result, a well-formed file, discipline
about what it touched, honesty about where its pixels came from, and a demonstrable answer to
the objective.

Every bound above is stated against what `refldx` achieves on the same input, and you have
`refldx`. Measure it, measure yourself, and you know your score before you are graded.
