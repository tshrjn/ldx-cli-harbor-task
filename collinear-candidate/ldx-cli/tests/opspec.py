"""tests/opspec.py -- programmatic conformance-grid spec for the LDX CLI.

The verifier generates its whole grid from this module: it asks for the tier of an
operation and for a handful of canonical invocations per operation, then runs each
one against both the reference binary and the agent's binary and compares.

Nothing here imports anything outside the standard library.

Public interface (see OPSPEC_INTERFACE.md):

    TIER_CORE, TIER_GEN, TIER_EXT, ALL_OPS   list[str]
    tier_of(op) -> "core" | "gen" | "ext"
    cases(op, rng, ctx) -> list[(argv, extra_inputs)]

Every flag name and every numeric bound below is taken from the reference source
(`ldx.c` plus `fam_layer.c`, `fam_tonal.c`, `fam_geom.c`, `fam_filter.c`,
`fam_docsel.c`, `fam_gen.c` and `fam_codec_cmds.c`): the `allowed[]` array at the top
of each `cmd_` function gives the accepted flags, and the `parse_int` / `arg_int` calls
give the valid ranges.

`ALL_OPS` is the 95 graded names, grouped by tier rather than in help order (help
interleaves the tiers -- `doc-flatten` precedes `layer-add`, and `px-convolve` sits
inside the filter family -- so the tier grouping the interface asks for cannot also
reproduce that order).  `refldx help` prints one further name, `gen-score`: it is a
measuring instrument for the generative tier, not a graded operation, and it is
deliberately absent from this module.

Nothing here assumes a command exists.  A command absent from the binary just exits
2 with `E_USAGE: unknown command`, which the verifier records like any other
mismatch; the module never inspects the binary and never raises because of one.
"""

from __future__ import annotations

import os
import random

# --------------------------------------------------------------------------
# Tiers
# --------------------------------------------------------------------------

# Tier C -- Core (PRD section 6, 12 table rows).  `doc-open` / `doc-save` share one
# row but are two command names, which is why this list holds 13 entries.
TIER_CORE = [
    "doc-new",
    "doc-open",
    "doc-save",
    "layer-add",
    "layer-set",
    "layer-reorder",
    "layer-merge-down",
    "layer-mask-apply",
    "doc-flatten",
    "px-crop",
    "px-transform",
    "px-channel",
    "px-convolve",
]

# Tier G -- Generative (algorithmic, seeded).
TIER_GEN = ["gen-fill", "gen-scale", "gen-heal", "gen-extend", "gen-retarget", "gen-denoise"]

# Tier E -- Extended: every remaining command printed by `refldx help`, in help order.
TIER_EXT = [
    "layer-delete", "layer-duplicate", "layer-rename", "layer-group",
    "layer-ungroup", "layer-merge-visible", "layer-mask-add", "layer-mask-remove",
    "layer-mask-invert", "layer-fill", "layer-clear", "layer-offset",
    "layer-copy", "layer-swap", "layer-opacity-scale", "layer-blend-set",
    "layer-lock-all", "px-invert", "px-threshold", "px-offset",
    "px-scale-value", "px-gamma", "px-brightness", "px-contrast",
    "px-levels", "px-posterize", "px-solarize", "px-desaturate",
    "px-channel-swap", "px-channel-extract", "px-channel-set", "px-channel-mix",
    "px-alpha-set", "px-alpha-multiply", "px-premultiply", "px-unpremultiply",
    "px-clamp", "px-flip-h", "px-flip-v", "px-rot90",
    "px-rot180", "px-rot270", "px-transpose", "px-scale",
    "px-pad", "px-translate", "px-tile", "px-blur-box",
    "px-blur-gauss", "px-sharpen", "px-unsharp", "px-edge",
    "px-emboss", "px-median", "px-erode", "px-dilate",
    "px-noise", "px-dither", "doc-info", "doc-resize",
    "doc-convert", "doc-trim", "doc-set-dpi", "doc-diff",
    "sel-rect", "sel-from-alpha", "sel-invert", "sel-apply",
    "stat-histogram", "stat-bbox", "stat-checksum", "stat-count",
    "doc-recompress", "doc-repack", "doc-stat-size", "doc-verify",
]

ALL_OPS = TIER_CORE + TIER_GEN + TIER_EXT

_TIER_OF = {}
for _o in TIER_CORE:
    _TIER_OF[_o] = "core"
for _o in TIER_GEN:
    _TIER_OF[_o] = "gen"
for _o in TIER_EXT:
    _TIER_OF[_o] = "ext"
del _o


def tier_of(op: str) -> str:
    """Return "core", "gen" or "ext" for one operation name."""
    try:
        return _TIER_OF[op]
    except KeyError:
        raise KeyError("unknown operation: %r" % (op,))


# --------------------------------------------------------------------------
# Constants mirrored from the reference source
# --------------------------------------------------------------------------

MAX_DIM = 16384          # ldx.c
MAX_LAYERS = 4096        # ldx.c
BLENDS = ("normal", "multiply", "screen", "darken", "lighten", "difference")
BAD_BLEND = "overlay"    # named by the PRD but absent from BLEND_NAMES[]
ANCHORS = ("tl", "tc", "tr", "cl", "cc", "cr", "bl", "bc", "br")
EMBOSS_DIRS = ("n", "ne", "e", "se", "s", "sw", "w", "nw")
TRANSFORM_OPS = ("flip_h", "flip_v", "rot90", "rot180", "rot270")
CHANNEL_OPS = ("invert", "threshold", "offset", "swap", "extract_alpha")
CODECS = ("none", "rle", "packbits", "delta")   # fam_codec.c CODEC_* ids 0..3
BAD_CODEC = "lzw"        # a plausible codec name that codec_from_name() rejects
MAX_INDEX = 65535        # arg_int(&a, "index", -1, -1, 65535) in the codec commands

# Expectation tags used internally (and by the self-test at the bottom of the file).
OK = "ok"     # must exit 0
ERR = "err"   # must exit non-zero, with the structured error code in the note


# --------------------------------------------------------------------------
# ctx handling
# --------------------------------------------------------------------------

def _resolve(ctx, key):
    """Best-effort absolute path for an asset named in ctx; None when unavailable."""
    raw = ctx.get(key)
    if not raw:
        return None
    p = str(raw)
    if os.path.isabs(p) and os.path.exists(p):
        return p
    bases = [ctx.get("assets_dir"), ctx.get("dir"), ctx.get("hidden")]
    f = ctx.get("file")
    if f and os.path.isabs(str(f)):
        bases.append(os.path.dirname(str(f)))
    for b in bases:
        if not b:
            continue
        q = os.path.join(str(b), p)
        if os.path.exists(q):
            return os.path.abspath(q)
    q = os.path.abspath(p)
    return q if os.path.exists(q) else None


class _Ctx(object):
    """Normalised view of the ctx dict, with defensive defaults."""

    def __init__(self, ctx):
        ctx = dict(ctx or {})
        self.raw = ctx
        self.w = max(1, int(ctx.get("w") or 8))
        self.h = max(1, int(ctx.get("h") or 8))
        self.mode = "gray" if str(ctx.get("mode") or "rgb") == "gray" else "rgb"
        self.rgb = self.mode == "rgb"
        self.alpha = 1 if int(ctx.get("alpha") or 0) else 0
        self.cc = 3 if self.rgb else 1
        self.ch = int(ctx.get("channels") or (self.cc + self.alpha))

        layers = ctx.get("layers")
        if layers is None:
            layers = [{"index": 0, "kind": "raster", "parent": -1}]
        self.kinds = []
        for i, rec in enumerate(layers):
            k = str(rec.get("kind", "raster")) if isinstance(rec, dict) else str(rec)
            if k not in ("raster", "group_open", "group_close", "mask"):
                k = "raster"
            self.kinds.append(k)
        self.n = len(self.kinds)

        self.rasters = [i for i, k in enumerate(self.kinds) if k == "raster"]
        self.groups = [i for i, k in enumerate(self.kinds) if k == "group_open"]
        self.closes = [i for i, k in enumerate(self.kinds) if k == "group_close"]
        self.masks = [i for i, k in enumerate(self.kinds) if k == "mask"]
        self.masked = [i for i in self.rasters if self.mask_of(i) >= 0]
        self.unmasked = [i for i in self.rasters if self.mask_of(i) < 0]

        self.file = _resolve(ctx, "file")
        self.pnm = _resolve(ctx, "pnm")
        self.alpha_pnm = _resolve(ctx, "alpha_pnm")

    # -- structural queries, mirroring the reference helpers ---------------

    def mask_of(self, idx):
        """lyr_mask_of(): the mask record attached to raster idx, or -1."""
        if 0 <= idx and idx + 1 < self.n and self.kinds[idx + 1] == "mask":
            return idx + 1
        return -1

    def close_of(self, idx):
        """group_close_of(): index of the matching group_close, or -1."""
        depth = 0
        for i in range(idx, self.n):
            if self.kinds[i] == "group_open":
                depth += 1
            elif self.kinds[i] == "group_close":
                depth -= 1
                if depth == 0:
                    return i
        return -1

    def span(self, idx):
        """lyr_span(): (lo, hi) for a raster (+ its mask) or a whole group."""
        if not (0 <= idx < self.n):
            return None
        if self.kinds[idx] == "raster":
            m = self.mask_of(idx)
            return (idx, m if m >= 0 else idx)
        if self.kinds[idx] == "group_open":
            c = self.close_of(idx)
            if c < 0:
                return None
            return (idx, c)
        return None

    def span_starts(self):
        """Indices lyr_span() accepts: raster and group_open records."""
        return [i for i in range(self.n) if self.kinds[i] in ("raster", "group_open")]

    def merge_down_targets(self):
        """Rasters that have a raster directly below them (skipping that one's mask)."""
        out = []
        for idx in self.rasters:
            below = idx - 1
            if below >= 0 and self.kinds[below] == "mask":
                below = idx - 2
            if below >= 0 and self.kinds[below] == "raster":
                out.append(idx)
        return out

    def reorder_pairs(self):
        """(from, to) pairs accepted by layer-reorder for this stack."""
        out = []
        for src in self.span_starts():
            sp = self.span(src)
            if sp is None:
                continue
            lo, hi = sp
            nrec = hi - lo + 1
            for to in range(0, self.n - nrec + 1):
                dest = to if to < lo else to + nrec
                if dest < self.n and self.kinds[dest] == "mask":
                    continue
                out.append((src, to))
        return out

    def swap_pairs(self):
        """(a, b) pairs accepted by layer-swap: two disjoint, whole spans."""
        out = []
        starts = self.span_starts()
        for i in range(len(starts)):
            for j in range(len(starts)):
                if i == j:
                    continue
                sa, sb = self.span(starts[i]), self.span(starts[j])
                if sa is None or sb is None:
                    continue
                (alo, ahi), (blo, bhi) = sa, sb
                if alo == blo:
                    continue
                if blo < alo:
                    (alo, ahi), (blo, bhi) = (blo, bhi), (alo, ahi)
                if blo <= ahi:
                    continue
                out.append((starts[i], starts[j]))
        return out

    def bad_index(self):
        """An --index value that is in parse range but names no record."""
        return min(self.n, MAX_LAYERS)

    def not_a_group(self):
        """An index that exists but is not a group_open (for --parent errors)."""
        for i in range(self.n):
            if self.kinds[i] != "group_open":
                return i
        return self.bad_index()

    def not_composable(self):
        """An index that is neither raster nor group_open, or an out-of-range one."""
        for i in range(self.n):
            if self.kinds[i] in ("group_close", "mask"):
                return i
        return self.bad_index()

    def not_raster(self):
        for i in range(self.n):
            if self.kinds[i] != "raster":
                return i
        return self.bad_index()


def _pick(rng, seq, default=None):
    return rng.choice(seq) if seq else default


def _fill(rng, n):
    return ",".join(str(rng.randrange(256)) for _ in range(n))


def _name(rng, prefix="L"):
    return "%s%d" % (prefix, rng.randrange(1000))


# --------------------------------------------------------------------------
# Case accumulator
# --------------------------------------------------------------------------

class _Cases(object):
    """Collects (argv, extra_inputs, expectation, note) tuples, capped at six."""

    LIMIT = 6

    def __init__(self):
        self.items = []

    def _add(self, argv, extra, expect, note):
        if len(self.items) >= self.LIMIT:
            return
        self.items.append((list(argv), dict(extra or {}), expect, note))

    # in.ldx -> out.ldx
    def ok(self, *argv, **kw):
        self._add(list(argv) + ["IN.ldx", "OUT.ldx"], kw.get("extra"), OK, kw.get("note", ""))

    def err(self, *argv, **kw):
        self._add(list(argv) + ["IN.ldx", "OUT.ldx"], kw.get("extra"), ERR, kw.get("note", ""))

    # in.ldx only (the command prints instead of writing a file)
    def ok_r(self, *argv, **kw):
        self._add(list(argv) + ["IN.ldx"], kw.get("extra"), OK, kw.get("note", ""))

    def err_r(self, *argv, **kw):
        self._add(list(argv) + ["IN.ldx"], kw.get("extra"), ERR, kw.get("note", ""))

    # fully explicit argv (doc-new, doc-diff, positional-count errors)
    def ok_raw(self, argv, extra=None, note=""):
        self._add(argv, extra, OK, note)

    def err_raw(self, argv, extra=None, note=""):
        self._add(argv, extra, ERR, note)


# ==========================================================================
# Tier C -- Core
# ==========================================================================

def _c_doc_new(rng, c):
    q = _Cases()
    mode = rng.choice(("rgb", "gray"))
    alpha = rng.randrange(2)
    ch = (3 if mode == "rgb" else 1) + alpha
    q.ok_raw(["doc-new", "--w", rng.randrange(1, 64), "--h", rng.randrange(1, 64),
              "--mode", mode, "--alpha", alpha, "--dpi", rng.randrange(1, 1200),
              "--fill", _fill(rng, ch), "OUT.ldx"], note="ordinary")
    # boundary: the extreme legal canvas in one axis, minimum dpi, no alpha
    q.ok_raw(["doc-new", "--w", MAX_DIM, "--h", 1, "--mode", "gray", "--alpha", 0,
              "--dpi", 1, "OUT.ldx"], note="boundary w=MAX_DIM dpi=1")
    q.ok_raw(["doc-new", "--w", 1, "--h", 1, "--mode", "rgb", "--alpha", 1,
              "--dpi", 65535, "--fill", "0,0,0,0", "OUT.ldx"], note="boundary 1x1 dpi=65535")
    q.err_raw(["doc-new", "--w", 2, "--h", 2, "--mode", "cmyk", "OUT.ldx"],
              note="E_BAD_ARGS: --mode must be gray or rgb")
    q.err_raw(["doc-new", "--w", 0, "--h", 2, "--mode", "rgb", "OUT.ldx"],
              note="E_BAD_ARGS: --w out of range (1..16384)")
    q.err_raw(["doc-new", "--w", 2, "--h", 2, "--mode", "indexed", "OUT.ldx"],
              note="E_MODE_UNSUPPORTED: indexed mode")
    return q


def _c_doc_open(rng, c):
    q = _Cases()
    q.ok_r("doc-open", note="ordinary")
    q.ok_r("doc-open", "--dump", note="boundary: full record dump")
    q.err_r("doc-open", "--fields", "document", note="E_USAGE: unknown flag --fields")
    q.err_raw(["doc-open"], note="E_USAGE: expected 1 positional, got 0")
    return q


def _c_doc_save(rng, c):
    q = _Cases()
    q.ok("doc-save", note="ordinary round-trip: every record keeps the codec it arrived with")
    q.ok("doc-save", "--compress", rng.choice(CODECS),
         note="boundary: store every record under one container codec")
    q.ok("doc-save", "--compress", "auto",
         note="boundary E19: the smallest encoding per record, ties to the lower codec id")
    q.err("doc-save", "--compress", BAD_CODEC,
          note="E_BAD_ARGS: --compress must be none, rle, packbits, delta or auto")
    q.err("doc-save", "--mode", "rgb", note="E_USAGE: unknown flag --mode")
    q.err_raw(["doc-save", "IN.ldx"], note="E_USAGE: expected 2 positionals, got 1")
    return q


def _c_layer_add(rng, c):
    q = _Cases()
    q.ok("layer-add", "--name", _name(rng), "--fill", _fill(rng, c.ch),
         "--opacity", rng.randrange(256), "--blend", rng.choice(BLENDS),
         "--visible", rng.randrange(2), "--locked", rng.randrange(2), note="ordinary raster")
    q.ok("layer-add", "--name", "G" + str(rng.randrange(100)), "--kind", "group",
         "--opacity", rng.randrange(256), note="ordinary group")
    # boundary: a 255-byte name, opacity extremes, fully transparent fill
    q.ok("layer-add", "--name", "N" * 255, "--opacity", 0, "--visible", 0,
         "--locked", 1, "--fill", ",".join(["0"] * c.ch), note="boundary 255-byte name")
    if c.groups:
        q.ok("layer-add", "--name", _name(rng, "Child"), "--parent", _pick(rng, c.groups),
             "--opacity", 255, note="boundary: nested under a group")
    elif c.pnm:
        args = ["layer-add", "--name", _name(rng, "Img"), "--from", "img.pnm"]
        extra = {"img.pnm": c.pnm}
        if c.alpha and c.alpha_pnm:
            args += ["--alpha-from", "alpha.pgm"]
            extra["alpha.pgm"] = c.alpha_pnm
        q.ok(*args, extra=extra, note="ordinary: pixels from a PNM")
    q.err("layer-add", "--name", "X", "--parent", c.not_a_group(),
          note="E_BAD_ARGS: --parent must name a group_open record")
    q.err("layer-add", "--name", "X", "--kind", "mask",
          note="E_UNSUPPORTED: use layer-mask-apply to add masks")
    return q


def _c_layer_set(rng, c):
    q = _Cases()
    tgt = _pick(rng, c.rasters + c.groups, 0)
    q.ok("layer-set", "--index", tgt, "--opacity", rng.randrange(256),
         "--blend", rng.choice(BLENDS), note="ordinary")
    q.ok("layer-set", "--index", tgt, "--opacity", 0, "--visible", 0, "--locked", 1,
         note="boundary opacity=0")
    q.ok("layer-set", "--index", tgt, "--opacity", 255, "--visible", 1, "--locked", 0,
         "--blend", "difference", note="boundary opacity=255")
    q.err("layer-set", "--index", tgt, note="E_USAGE: needs at least one attribute flag")
    q.err("layer-set", "--index", tgt, "--opacity", 256,
          note="E_BAD_ARGS: --opacity out of range (0..255)")
    q.err("layer-set", "--index", c.not_composable(), "--opacity", 128,
          note="E_BAD_ARGS: --index must name a raster or group_open record")
    return q


def _c_layer_reorder(rng, c):
    q = _Cases()
    pairs = c.reorder_pairs()
    moves = [p for p in pairs if p[0] != p[1]] or pairs
    if moves:
        f, t = _pick(rng, moves)
        q.ok("layer-reorder", "--from", f, "--to", t, note="ordinary")
        bottom = [p for p in pairs if p[1] == 0]
        if bottom:
            f2, t2 = bottom[0]
            q.ok("layer-reorder", "--from", f2, "--to", t2, note="boundary: move to the bottom")
        top = sorted(pairs, key=lambda p: -p[1])
        if top and (top[0] != (f, t)):
            q.ok("layer-reorder", "--from", top[0][0], "--to", top[0][1],
                 note="boundary: move to the top")
    q.err("layer-reorder", "--from", _pick(rng, c.span_starts(), 0), "--to", max(c.n, 1),
          note="E_BAD_ARGS: --to out of range for this record and its children")
    q.err("layer-reorder", "--from", c.bad_index(), "--to", 0,
          note="E_BAD_ARGS: --from out of range")
    return q


def _c_layer_merge_down(rng, c):
    q = _Cases()
    tgts = c.merge_down_targets()
    if tgts:
        q.ok("layer-merge-down", "--index", _pick(rng, tgts), note="ordinary")
        q.ok("layer-merge-down", "--index", max(tgts), note="boundary: topmost mergeable layer")
    q.err("layer-merge-down", "--index", 0,
          note="E_BAD_ARGS: no raster layer directly below index 0")
    q.err("layer-merge-down", "--index", c.bad_index(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    return q


def _c_layer_mask_apply(rng, c):
    q = _Cases()
    if c.masked and c.alpha:
        q.ok("layer-mask-apply", "--index", _pick(rng, c.masked), note="ordinary (E06 order)")
        q.ok("layer-mask-apply", "--index", min(c.masked), note="boundary: lowest masked layer")
    elif c.masked and not c.alpha:
        q.err("layer-mask-apply", "--index", _pick(rng, c.masked),
              note="E_MODE_UNSUPPORTED: no alpha channel to bake the mask into")
    if c.unmasked:
        q.err("layer-mask-apply", "--index", _pick(rng, c.unmasked),
              note="E_BAD_ARGS: layer has no mask")
    q.err("layer-mask-apply", "--index", c.bad_index(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    q.err("layer-mask-apply", "--index", 0, "--mask", 1,
          note="E_USAGE: unknown flag --mask")
    return q


def _c_doc_flatten(rng, c):
    q = _Cases()
    q.ok("doc-flatten", note="ordinary (E08 empty group, E11 opacity compounding)")
    q.err("doc-flatten", "--index", 0, note="E_USAGE: doc-flatten accepts no flags")
    q.err_raw(["doc-flatten", "IN.ldx"], note="E_USAGE: expected 2 positionals, got 1")
    return q


def _c_px_crop(rng, c):
    q = _Cases()
    x = rng.randrange(0, c.w)
    y = rng.randrange(0, c.h)
    q.ok("px-crop", "--x", x, "--y", y, "--w", rng.randrange(1, c.w - x + 1),
         "--h", rng.randrange(1, c.h - y + 1), note="ordinary")
    q.ok("px-crop", "--x", 0, "--y", 0, "--w", c.w, "--h", c.h,
         note="boundary: the whole canvas")
    q.ok("px-crop", "--x", -1, "--y", -1, "--w", c.w + 2, "--h", c.h + 2,
         note="boundary E07: clamped, W_CROP_CLAMPED, exit 0")
    q.err("px-crop", "--x", c.w, "--y", 0, "--w", 1, "--h", 1,
          note="E_BAD_RECT: rectangle does not intersect the canvas")
    q.err("px-crop", "--x", 0, "--y", 0, "--w", 0, "--h", 1,
          note="E_BAD_ARGS: --w out of range (1..16384)")
    return q


def _c_px_transform(rng, c):
    q = _Cases()
    q.ok("px-transform", "--op", rng.choice(TRANSFORM_OPS), note="ordinary")
    q.ok("px-transform", "--op", "rot270", note="boundary E09: rot270 == rot90 x3")
    q.ok("px-transform", "--op", "flip_v", note="boundary")
    q.err("px-transform", "--op", "transpose",
          note="E_BAD_ARGS: transpose is not one of px-transform's ops")
    q.err("px-transform", "--op", "rot90", "--index", 0,
          note="E_USAGE: unknown flag --index")
    return q


def _c_px_channel(rng, c):
    q = _Cases()
    idx = _pick(rng, c.rasters, 0)
    q.ok("px-channel", "--op", "invert", "--index", idx, note="ordinary")
    q.ok("px-channel", "--op", "threshold", "--arg", rng.randrange(256), "--index", idx,
         note="ordinary threshold")
    q.ok("px-channel", "--op", "offset", "--arg", rng.choice((-255, 255)), "--index", idx,
         note="boundary E03: offset saturates, never wraps")
    if c.rgb:
        q.ok("px-channel", "--op", "swap", "--index", idx, note="ordinary rgb swap")
    else:
        q.err("px-channel", "--op", "swap", "--index", idx,
              note="E_MODE_UNSUPPORTED (E10): swap requires an rgb document")
    if c.alpha:
        q.ok("px-channel", "--op", "extract_alpha", "--index", idx, note="boundary alpha")
    q.err("px-channel", "--op", "sharpen", "--index", idx,
          note="E_BAD_ARGS: --op must be invert, threshold, offset, swap or extract_alpha")
    return q


def _c_px_convolve(rng, c):
    q = _Cases()
    k = [rng.randrange(-64, 65) for _ in range(8)]
    k.append(256 - sum(k))  # DC-preserving 3x3 at shift 8
    q.ok("px-convolve", "--kernel", ",".join(str(v) for v in k), "--shift", 8,
         "--index", _pick(rng, c.rasters, 0), note="ordinary 3x3 (E04 tie rounding)")
    k5 = ["1"] * 25
    q.ok("px-convolve", "--kernel", ",".join(k5), "--shift", rng.randrange(0, 5),
         "--bias", rng.randrange(-255, 256), "--all", note="boundary 5x5 kernel, --all")
    q.ok("px-convolve", "--kernel", "32767,0,0,0,-32768,0,0,0,0", "--shift", 30,
         "--bias", 255, "--index", _pick(rng, c.rasters, 0),
         note="boundary E03: extreme taps, shift 30, bias 255")
    q.ok("px-convolve", "--kernel", "0,0,0,0,1,0,0,0,0", "--shift", 0, "--bias", -255,
         "--index", _pick(rng, c.rasters, 0), note="boundary: shift 0, bias -255")
    q.err("px-convolve", "--kernel", "1,2,3,4,5", "--shift", 8,
          note="E_BAD_ARGS: --kernel must have 9 or 25 values")
    q.err("px-convolve", "--kernel", "0,0,0,0,1,0,0,0,0", "--shift", 31,
          note="E_BAD_ARGS: --shift out of range (0..30)")
    return q


# ==========================================================================
# Tier G -- Generative (fam_gen.c; every one takes a required --seed)
# ==========================================================================

MAX_SEED = 2147483647  # parse_int bound shared by all four gen-* commands


def _seed(rng):
    return rng.randrange(0, MAX_SEED + 1)


def _fill_region(c, radius):
    """A rectangle at the origin that leaves at least one legal gen-fill patch.

    gen-fill needs a pixel (x, y) whose whole (2r+1)^2 patch is inside the canvas
    and disjoint from the region.  With the region pinned to the top-left corner as
    [0, rw) x [0, rh), the pixel (rw + r, r) qualifies whenever
    rw + 2r + 1 <= w and 2r + 1 <= h.  Returns (rw, rh) or None.
    """
    if c.w < 2 * radius + 2 or c.h < 2 * radius + 1:
        return None
    return (max(1, min(c.w - 1 - 2 * radius, max(1, c.w // 3))), max(1, c.h // 3))


def _g_gen_fill(rng, c):
    q = _Cases()
    idx = _pick(rng, c.rasters, 0)
    radii = [r for r in (2, 1, 0) if _fill_region(c, r)]
    if radii:
        r = radii[0]
        rw, rh = _fill_region(c, r)
        q.ok("gen-fill", "--seed", _seed(rng), "--index", idx, "--radius", r,
             "--iters", rng.randrange(1, 9), "--x", 0, "--y", 0, "--w", rw, "--h", rh,
             note="ordinary fill over an explicit rectangle")
        rw0, rh0 = _fill_region(c, 0)
        q.ok("gen-fill", "--seed", 0, "--index", idx, "--radius", 0, "--iters", 1,
             "--x", 0, "--y", 0, "--w", rw0, "--h", rh0,
             note="boundary: radius 0, one iteration, seed 0")
        q.ok("gen-fill", "--seed", MAX_SEED, "--index", idx, "--radius", 0, "--iters", 64,
             "--x", 0, "--y", 0, "--w", rw0, "--h", rh0,
             note="boundary: 64 iterations, maximum seed")
    q.err("gen-fill", "--index", idx, "--x", 0, "--y", 0, "--w", 1, "--h", 1,
          note="E_USAGE: --seed is required for every gen-* command")
    q.err("gen-fill", "--seed", _seed(rng), "--index", idx, "--x", 0,
          note="E_BAD_ARGS: --x --y --w --h must be given together")
    q.err("gen-fill", "--seed", MAX_SEED + 1, "--index", idx, "--x", 0, "--y", 0,
          "--w", 1, "--h", 1, note="E_BAD_ARGS: --seed out of range (0..2147483647)")
    return q


def _g_gen_scale(rng, c):
    q = _Cases()
    grow = min(MAX_DIM, c.w + rng.randrange(1, 4))
    q.ok("gen-scale", "--seed", _seed(rng), "--w", max(1, c.w - 1),
         "--jitter", rng.randrange(0, 256), note="ordinary seam carve: one seam, so the energy bound is absolute")
    q.ok("gen-scale", "--seed", 0, "--w", 1, "--jitter", 0,
         note="boundary: carve down to a single column, jitter 0")
    q.ok("gen-scale", "--seed", MAX_SEED, "--w", grow, "--jitter", 255,
         note="boundary: seam insertion, maximum seed and jitter")
    q.ok("gen-scale", "--seed", _seed(rng), "--w", max(1, c.w - 1), "--jitter", 0,
         note="one seam at jitter 0: the seam must be a minimum-energy seam, exactly")
    q.ok("gen-scale", "--seed", _seed(rng), "--w", c.w, note="boundary: no-op width")
    q.err("gen-scale", "--w", max(1, c.w - 1), note="E_USAGE: --seed is required")
    q.err("gen-scale", "--seed", _seed(rng), "--w", 0,
          note="E_BAD_ARGS: --w out of range (1..16384)")
    return q


def _g_gen_heal(rng, c):
    q = _Cases()
    idx = _pick(rng, c.rasters, 0)
    x = rng.randrange(0, c.w)
    y = rng.randrange(0, c.h)
    q.ok("gen-heal", "--seed", _seed(rng), "--index", idx, "--radius", rng.randrange(1, 17),
         "--x", x, "--y", y, "--w", rng.randrange(1, c.w - x + 1),
         "--h", rng.randrange(1, c.h - y + 1),
         note="ordinary membrane solve")
    q.ok("gen-heal", "--seed", 0, "--index", idx, "--radius", 1,
         "--x", 0, "--y", 0, "--w", 1, "--h", 1,
         note="boundary: one-pixel region, radius 1, seed 0")
    q.ok("gen-heal", "--seed", MAX_SEED, "--index", idx, "--radius", MAX_DIM,
         "--x", 0, "--y", 0, "--w", c.w, "--h", c.h,
         note="boundary: whole canvas, maximum radius and seed")
    q.ok("gen-heal", "--seed", _seed(rng), "--index", idx, "--x", -1, "--y", -1,
         "--w", c.w + 2, "--h", c.h + 2,
         note="boundary E07: region clamped, W_CROP_CLAMPED, exit 0")
    q.err("gen-heal", "--index", idx, "--x", 0, "--y", 0, "--w", 1, "--h", 1,
          note="E_USAGE: --seed is required")
    q.err("gen-heal", "--seed", _seed(rng), "--index", idx, "--x", c.w, "--y", 0,
          "--w", 1, "--h", 1, note="E_BAD_RECT: region does not intersect the canvas")
    return q


def _g_gen_extend(rng, c):
    q = _Cases()
    q.ok("gen-extend", "--seed", _seed(rng),
         "--dir", rng.choice(("left", "right", "top", "bottom")),
         "--amount", rng.randrange(1, 9), "--radius", rng.randrange(1, 33),
         note="ordinary directional synthesis")
    q.ok("gen-extend", "--seed", 0, "--dir", "right", "--amount", 1, "--radius", 1,
         note="boundary: one new column, radius 1, seed 0")
    q.ok("gen-extend", "--seed", MAX_SEED, "--dir", "bottom", "--amount", c.h,
         "--radius", MAX_DIM, note="boundary: double the height, maximum radius and seed")
    q.err("gen-extend", "--dir", "right", "--amount", 1, note="E_USAGE: --seed is required")
    q.err("gen-extend", "--seed", _seed(rng), "--dir", "sideways", "--amount", 1,
          note="E_BAD_ARGS: --dir must be left, right, top or bottom")
    q.err("gen-extend", "--seed", _seed(rng), "--dir", "right", "--amount", MAX_DIM,
          note="E_BAD_ARGS: resulting canvas exceeds the maximum dimension")
    return q


def _g_gen_retarget(rng, c):
    q = _Cases()
    grow = min(MAX_DIM, c.h + rng.randrange(1, 4))
    q.ok("gen-retarget", "--seed", _seed(rng), "--h", max(1, c.h - 1), "--jitter", 0,
         note="one seam at jitter 0: the seam must be a minimum-energy horizontal seam, exactly")
    q.ok("gen-retarget", "--seed", _seed(rng), "--h", max(1, c.h - 1),
         "--jitter", rng.randrange(0, 256),
         note="ordinary seam carve: one seam, so the energy bound is absolute")
    q.ok("gen-retarget", "--seed", 0, "--h", 1, "--jitter", 0,
         note="boundary: carve down to a single row, jitter 0")
    q.ok("gen-retarget", "--seed", MAX_SEED, "--h", grow, "--jitter", 255,
         note="boundary: seam insertion, maximum seed and jitter")
    q.err("gen-retarget", "--h", max(1, c.h - 1), note="E_USAGE: --seed is required")
    q.err("gen-retarget", "--seed", _seed(rng), "--h", 0,
          note="E_BAD_ARGS: --h out of range (1..16384)")
    return q


def _g_gen_denoise(rng, c):
    q = _Cases()
    idx = _pick(rng, c.rasters, 0)
    q.ok("gen-denoise", "--seed", _seed(rng), "--index", idx,
         "--radius", rng.randrange(1, 5), "--strength", rng.randrange(1, 65),
         note="ordinary non-local means")
    q.ok("gen-denoise", "--seed", 0, "--index", idx, "--radius", 0, "--strength", 0,
         note="boundary: radius 0 and strength 0 are both the identity")
    q.ok("gen-denoise", "--seed", MAX_SEED, "--index", (max(c.rasters) if c.rasters else 0),
         "--radius", 16, "--strength", 255,
         note="boundary: maximum window, maximum strength, topmost raster")
    q.err("gen-denoise", "--index", idx, note="E_USAGE: --seed is required")
    q.err("gen-denoise", "--seed", _seed(rng), "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    q.err("gen-denoise", "--seed", _seed(rng), "--index", idx, "--strength", 256,
          note="E_BAD_ARGS: --strength out of range (0..255)")
    return q


# ==========================================================================
# Tier E -- layer family
# ==========================================================================

def _e_layer_delete(rng, c):
    q = _Cases()
    starts = c.span_starts()
    if starts:
        q.ok("layer-delete", "--index", _pick(rng, starts), note="ordinary")
        q.ok("layer-delete", "--index", max(starts), note="boundary: topmost record")
    q.err("layer-delete", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    q.err("layer-delete", "--index", c.not_composable(),
          note="E_BAD_ARGS: --index must name a raster or group_open record")
    return q


def _e_layer_duplicate(rng, c):
    q = _Cases()
    starts = c.span_starts()
    if starts:
        q.ok("layer-duplicate", "--index", _pick(rng, starts), note="ordinary")
        q.ok("layer-duplicate", "--index", min(starts), "--name", "C" * 255,
             note="boundary: 255-byte name on the copy")
    q.err("layer-duplicate", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    q.err("layer-duplicate", "--index", _pick(rng, starts, 0), "--name", "",
          note="E_BAD_ARGS: --name must be 1..255 bytes")
    return q


def _e_layer_rename(rng, c):
    q = _Cases()
    renamable = [i for i in range(c.n) if c.kinds[i] != "group_close"]
    if renamable:
        q.ok("layer-rename", "--index", _pick(rng, renamable), "--name", _name(rng, "R"),
             note="ordinary")
        q.ok("layer-rename", "--index", _pick(rng, renamable), "--name", "Z" * 255,
             note="boundary: 255-byte name")
        q.ok("layer-rename", "--index", _pick(rng, renamable), "--name", "~",
             note="boundary: single printable byte")
    q.err("layer-rename", "--index", _pick(rng, renamable, 0), "--name", "",
          note="E_BAD_ARGS: --name must be 1..255 bytes")
    if c.closes:
        q.err("layer-rename", "--index", c.closes[0], "--name", "X",
              note="E_BAD_ARGS: group_close records carry no name")
    else:
        q.err("layer-rename", "--index", c.bad_index(), "--name", "X",
              note="E_BAD_ARGS: --index out of range")
    return q


def _e_layer_group(rng, c):
    q = _Cases()
    if c.rasters:
        i = _pick(rng, c.rasters)
        q.ok("layer-group", "--from", i, "--to", i, "--name", _name(rng, "Grp"),
             note="ordinary: wrap one raster (widened over its mask)")
        q.ok("layer-group", "--from", min(c.rasters), "--to", min(c.rasters),
             "--name", "G" * 255, note="boundary: 255-byte group name")
    if c.groups:
        g = c.groups[0]
        cl = c.close_of(g)
        if cl >= 0:
            q.ok("layer-group", "--from", g, "--to", cl, "--name", _name(rng, "Outer"),
                 note="boundary: wrap a whole balanced group")
    if c.n >= 2:
        q.err("layer-group", "--from", 1, "--to", 0, "--name", "X",
              note="E_BAD_ARGS: --to must not be smaller than --from")
    q.err("layer-group", "--from", c.bad_index(), "--to", c.bad_index(), "--name", "X",
          note="E_BAD_ARGS: --from/--to out of range")
    return q


def _e_layer_ungroup(rng, c):
    q = _Cases()
    if c.groups:
        q.ok("layer-ungroup", "--index", _pick(rng, c.groups), note="ordinary (E08)")
        q.ok("layer-ungroup", "--index", max(c.groups), note="boundary: innermost group")
    q.err("layer-ungroup", "--index", c.not_a_group(),
          note="E_BAD_ARGS: --index must be the index of a group_open record")
    q.err("layer-ungroup", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    return q


def _e_layer_merge_visible(rng, c):
    q = _Cases()
    q.ok("layer-merge-visible", note="ordinary (E08 empty groups, E11 compounding)")
    q.err("layer-merge-visible", "--index", 0,
          note="E_USAGE: layer-merge-visible accepts no flags")
    q.err_raw(["layer-merge-visible", "IN.ldx"], note="E_USAGE: expected 2 positionals, got 1")
    return q


def _e_layer_mask_add(rng, c):
    q = _Cases()
    if c.unmasked:
        i = _pick(rng, c.unmasked)
        q.ok("layer-mask-add", "--index", i, note="ordinary: opaque mask")
        q.ok("layer-mask-add", "--index", min(c.unmasked), "--fill", 0,
             note="boundary: fully hidden mask")
        q.ok("layer-mask-add", "--index", max(c.unmasked), "--fill", 255,
             note="boundary: fully opaque mask")
        if c.alpha_pnm:
            q.ok("layer-mask-add", "--index", i, "--from", "mask.pgm",
                 extra={"mask.pgm": c.alpha_pnm}, note="ordinary: mask from a P5 file")
    if c.masked:
        q.err("layer-mask-add", "--index", _pick(rng, c.masked),
              note="E_BAD_ARGS: layer already has a mask")
    q.err("layer-mask-add", "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    return q


def _e_layer_mask_remove(rng, c):
    q = _Cases()
    if c.masked:
        q.ok("layer-mask-remove", "--index", _pick(rng, c.masked), note="ordinary")
        q.ok("layer-mask-remove", "--index", min(c.masked), note="boundary: lowest masked layer")
    if c.unmasked:
        q.err("layer-mask-remove", "--index", _pick(rng, c.unmasked),
              note="E_BAD_ARGS: layer has no mask")
    q.err("layer-mask-remove", "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    return q


def _e_layer_mask_invert(rng, c):
    q = _Cases()
    if c.masked:
        q.ok("layer-mask-invert", "--index", _pick(rng, c.masked), note="ordinary")
        q.ok("layer-mask-invert", "--index", max(c.masked), note="boundary: highest masked layer")
    if c.unmasked:
        q.err("layer-mask-invert", "--index", _pick(rng, c.unmasked),
              note="E_BAD_ARGS: layer has no mask")
    q.err("layer-mask-invert", "--index", c.bad_index(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    return q


def _e_layer_fill(rng, c):
    q = _Cases()
    i = _pick(rng, c.rasters, 0)
    q.ok("layer-fill", "--index", i, "--colour", _fill(rng, c.ch), note="ordinary")
    q.ok("layer-fill", "--index", i, "--colour", ",".join(["0"] * c.ch),
         note="boundary: all channels 0")
    q.ok("layer-fill", "--index", i, "--colour", ",".join(["255"] * c.ch),
         note="boundary: all channels 255")
    q.err("layer-fill", "--index", i, "--colour", ",".join(["1"] * (c.ch + 1)),
          note="E_BAD_ARGS: --fill needs one component per channel")
    q.err("layer-fill", "--index", c.not_raster(), "--colour", _fill(rng, c.ch),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    return q


def _e_layer_clear(rng, c):
    q = _Cases()
    if c.rasters:
        q.ok("layer-clear", "--index", _pick(rng, c.rasters), note="ordinary")
        q.ok("layer-clear", "--index", max(c.rasters), note="boundary: topmost raster")
    q.err("layer-clear", "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    q.err("layer-clear", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    return q


def _e_layer_offset(rng, c):
    q = _Cases()
    i = _pick(rng, c.rasters, 0)
    q.ok("layer-offset", "--index", i, "--dx", rng.randrange(-c.w, c.w + 1),
         "--dy", rng.randrange(-c.h, c.h + 1), "--wrap", rng.randrange(2), note="ordinary")
    q.ok("layer-offset", "--index", i, "--dx", MAX_DIM, "--dy", -MAX_DIM, "--wrap", 1,
         note="boundary: extreme shift, wrapped")
    q.ok("layer-offset", "--index", i, "--dx", -MAX_DIM, "--dy", MAX_DIM, "--wrap", 0,
         note="boundary: extreme shift, zero-filled")
    q.err("layer-offset", "--index", i, "--dx", 1, "--dy", 1, "--wrap", 2,
          note="E_BAD_ARGS: --wrap out of range (0..1)")
    q.err("layer-offset", "--index", i, "--dx", MAX_DIM + 1,
          note="E_BAD_ARGS: --dx out of range (-16384..16384)")
    return q


def _e_layer_copy(rng, c):
    q = _Cases()
    if len(c.rasters) >= 2:
        a, b = rng.sample(c.rasters, 2)
        q.ok("layer-copy", "--from", a, "--to", b, note="ordinary raster -> raster")
    if c.rasters:
        s = _pick(rng, c.rasters)
        q.ok("layer-copy", "--from", s, "--to", s, note="boundary: copy onto itself")
    if len(c.masks) >= 2:
        a, b = rng.sample(c.masks, 2)
        q.ok("layer-copy", "--from", a, "--to", b, note="boundary mask -> mask")
    mismatch = c.masks + c.groups + c.closes
    if c.rasters and mismatch:
        q.err("layer-copy", "--from", _pick(rng, c.rasters), "--to", _pick(rng, mismatch),
              note="E_BAD_ARGS: --from and --to must be the same kind")
    q.err("layer-copy", "--from", 0, "--to", c.bad_index(),
          note="E_BAD_ARGS: --from/--to out of range")
    return q


def _e_layer_swap(rng, c):
    q = _Cases()
    pairs = c.swap_pairs()
    if pairs:
        a, b = _pick(rng, pairs)
        q.ok("layer-swap", "--a", a, "--b", b, note="ordinary")
        a2, b2 = pairs[0]
        q.ok("layer-swap", "--a", b2, "--b", a2, note="boundary: reversed order")
    starts = c.span_starts()
    if starts:
        s = _pick(rng, starts)
        q.ok("layer-swap", "--a", s, "--b", s, note="boundary: swap a record with itself")
    # a group and a record inside it overlap and must be rejected
    overlap = None
    for g in c.groups:
        cl = c.close_of(g)
        for i in range(g + 1, cl):
            if c.kinds[i] in ("raster", "group_open"):
                overlap = (g, i)
                break
        if overlap:
            break
    if overlap:
        q.err("layer-swap", "--a", overlap[0], "--b", overlap[1],
              note="E_BAD_ARGS: a group cannot be swapped with its own child")
    q.err("layer-swap", "--a", 0, "--b", c.bad_index(), note="E_BAD_ARGS: --b out of range")
    return q


def _e_layer_opacity_scale(rng, c):
    q = _Cases()
    i = _pick(rng, c.rasters + c.groups, 0)
    q.ok("layer-opacity-scale", "--index", i, "--num", rng.randrange(0, 512),
         "--den", rng.randrange(1, 512), note="ordinary")
    q.ok("layer-opacity-scale", "--index", i, "--num", 65535, "--den", 1,
         note="boundary: clamps to 255 (E03)")
    q.ok("layer-opacity-scale", "--index", i, "--num", 0, "--den", 65535,
         note="boundary: scales to 0")
    q.err("layer-opacity-scale", "--index", i, "--num", 1, "--den", 0,
          note="E_BAD_ARGS: --den out of range (1..65535)")
    q.err("layer-opacity-scale", "--index", c.not_composable(), "--num", 1, "--den", 2,
          note="E_BAD_ARGS: --index must name a raster or group_open record")
    return q


def _e_layer_blend_set(rng, c):
    q = _Cases()
    i = _pick(rng, c.rasters + c.groups, 0)
    q.ok("layer-blend-set", "--index", i, "--blend", rng.choice(BLENDS), note="ordinary")
    q.ok("layer-blend-set", "--index", i, "--blend", BLENDS[0], note="boundary: first blend mode")
    q.ok("layer-blend-set", "--index", i, "--blend", BLENDS[-1], note="boundary: last blend mode")
    q.err("layer-blend-set", "--index", i, "--blend", BAD_BLEND,
          note="E_BAD_ARGS: unknown blend mode")
    q.err("layer-blend-set", "--index", c.not_composable(), "--blend", "normal",
          note="E_BAD_ARGS: --index must name a raster or group_open record")
    return q


def _e_layer_lock_all(rng, c):
    q = _Cases()
    q.ok("layer-lock-all", "--locked", 1, note="ordinary: lock everything")
    q.ok("layer-lock-all", "--locked", 0, note="boundary: unlock everything")
    q.err("layer-lock-all", "--locked", 2, note="E_BAD_ARGS: --locked out of range (0..1)")
    q.err("layer-lock-all", note="E_USAGE: missing required flag --locked")
    return q


# ==========================================================================
# Tier E -- tonal family (all take --index / --all)
# ==========================================================================

def _sel(rng, c):
    """A random valid layer selector for the tonal / filter engines."""
    if c.rasters and rng.random() < 0.7:
        return ["--index", _pick(rng, c.rasters)]
    return ["--all"]


def _lut_cases(q, rng, c, op, arg_variants, err_variants):
    """Shared shape for the LUT commands: ordinary, boundaries, selector errors."""
    for i, args in enumerate(arg_variants):
        note = "ordinary" if i == 0 else "boundary"
        if i == 0:
            q.ok(op, *(list(args) + _sel(rng, c)), note=note)
        elif i == 1:
            q.ok(op, *(list(args) + ["--all"]), note=note + " (--all)")
        else:
            q.ok(op, *(list(args) + ["--index", _pick(rng, c.rasters, 0)]), note=note)
    for args, note in err_variants:
        q.err(op, *args, note=note)


def _e_px_invert(rng, c):
    q = _Cases()
    q.ok("px-invert", *_sel(rng, c), note="ordinary")
    q.ok("px-invert", "--all", note="boundary: every raster record")
    q.ok("px-invert", "--index", max(c.rasters) if c.rasters else 0,
         note="boundary: topmost raster")
    q.err("px-invert", "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    q.err("px-invert", "--all", "--index", 0,
          note="E_BAD_ARGS: --all and --index are mutually exclusive")
    return q


def _e_px_threshold(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-threshold",
               [["--arg", rng.randrange(256)], ["--arg", 0], ["--arg", 255]],
               [(["--arg", 256, "--index", 0], "E_BAD_ARGS: --arg out of range (0..255)"),
                (["--index", c.not_raster()], "E_USAGE: missing required flag --arg")])
    return q


def _e_px_offset(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-offset",
               [["--arg", rng.randrange(-255, 256)], ["--arg", -255], ["--arg", 255]],
               [(["--arg", 256, "--index", 0], "E_BAD_ARGS: --arg out of range (-255..255)"),
                (["--arg", 0, "--index", c.not_raster()],
                 "E_BAD_ARGS: --index must be the index of a raster layer")])
    return q


def _e_px_scale_value(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-scale-value",
               [["--num", rng.randrange(0, 512), "--den", rng.randrange(1, 512)],
                ["--num", 65535, "--den", 1],
                ["--num", 0, "--den", 65535]],
               [(["--num", 1, "--den", 0, "--index", 0],
                 "E_BAD_ARGS: --den out of range (1..65535)"),
                (["--num", 65536, "--index", 0],
                 "E_BAD_ARGS: --num out of range (0..65535)")])
    return q


def _e_px_gamma(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-gamma",
               [["--num", rng.randrange(1, 64), "--den", rng.randrange(1, 64)],
                ["--num", 1024, "--den", 1],
                ["--num", 1, "--den", 1024]],
               [(["--num", 0, "--den", 1, "--index", 0],
                 "E_BAD_ARGS: --num out of range (1..1024)"),
                (["--num", 1, "--den", 1025, "--index", 0],
                 "E_BAD_ARGS: --den out of range (1..1024)")])
    return q


def _e_px_brightness(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-brightness",
               [["--arg", rng.randrange(-255, 256)], ["--arg", -255], ["--arg", 255]],
               [(["--arg", -256, "--index", 0], "E_BAD_ARGS: --arg out of range (-255..255)"),
                (["--index", 0], "E_USAGE: missing required flag --arg")])
    return q


def _e_px_contrast(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-contrast",
               [["--arg", rng.randrange(-100, 201)], ["--arg", -100], ["--arg", 1000]],
               [(["--arg", -101, "--index", 0], "E_BAD_ARGS: --arg out of range (-100..1000)"),
                (["--arg", 1001, "--index", 0], "E_BAD_ARGS: --arg out of range (-100..1000)")])
    return q


def _e_px_levels(rng, c):
    q = _Cases()
    inb = rng.randrange(0, 128)
    inw = rng.randrange(inb + 1, 256)
    _lut_cases(q, rng, c, "px-levels",
               [["--in-black", inb, "--in-white", inw,
                 "--out-black", rng.randrange(256), "--out-white", rng.randrange(256)],
                ["--in-black", 0, "--in-white", 255, "--out-black", 255, "--out-white", 0],
                ["--in-black", 0, "--in-white", 1, "--out-black", 0, "--out-white", 255]],
               [(["--in-black", 200, "--in-white", 200, "--index", 0],
                 "E_BAD_ARGS: --in-black must be below --in-white"),
                (["--in-black", 256, "--index", 0],
                 "E_BAD_ARGS: --in-black out of range (0..255)")])
    return q


def _e_px_posterize(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-posterize",
               [["--levels", rng.randrange(2, 64)], ["--levels", 2], ["--levels", 256]],
               [(["--levels", 1, "--index", 0], "E_BAD_ARGS: --levels out of range (2..256)"),
                (["--levels", 257, "--index", 0], "E_BAD_ARGS: --levels out of range (2..256)")])
    return q


def _e_px_solarize(rng, c):
    q = _Cases()
    _lut_cases(q, rng, c, "px-solarize",
               [["--arg", rng.randrange(256)], ["--arg", 0], ["--arg", 255]],
               [(["--arg", 256, "--index", 0], "E_BAD_ARGS: --arg out of range (0..255)"),
                (["--index", 0], "E_USAGE: missing required flag --arg")])
    return q


def _e_px_desaturate(rng, c):
    q = _Cases()
    if c.rgb:
        q.ok("px-desaturate", *_sel(rng, c), note="ordinary")
        q.ok("px-desaturate", "--all", note="boundary: every raster record")
        q.err("px-desaturate", "--index", c.not_raster(),
              note="E_BAD_ARGS: --index must be the index of a raster layer")
    else:
        q.err("px-desaturate", "--index", _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED (E10): requires an rgb document")
        q.err("px-desaturate", "--all",
              note="E_MODE_UNSUPPORTED (E10): requires an rgb document")
    q.err("px-desaturate", "--all", "--index", 0,
          note="E_BAD_ARGS: --all and --index are mutually exclusive")
    return q


def _e_px_channel_swap(rng, c):
    q = _Cases()
    if c.rgb:
        pair = rng.choice((("r", "g"), ("g", "b"), ("r", "b")))
        q.ok("px-channel-swap", "--a", pair[0], "--b", pair[1], *_sel(rng, c), note="ordinary")
        q.ok("px-channel-swap", "--a", "r", "--b", "r", "--all",
             note="boundary: swapping a channel with itself")
        if c.alpha:
            q.ok("px-channel-swap", "--a", "r", "--b", "a", "--index",
                 _pick(rng, c.rasters, 0), note="boundary: colour <-> alpha")
        else:
            q.err("px-channel-swap", "--a", "r", "--b", "a", "--index",
                  _pick(rng, c.rasters, 0),
                  note="E_MODE_UNSUPPORTED: document has no alpha channel")
    else:
        q.err("px-channel-swap", "--a", "r", "--b", "b", "--index", _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED (E10): requires an rgb document")
        q.err("px-channel-swap", "--a", "r", "--b", "g", "--all",
              note="E_MODE_UNSUPPORTED (E10): requires an rgb document")
    q.err("px-channel-swap", "--a", "r", "--b", "z", "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS / E_MODE_UNSUPPORTED: channel must be r, g, b or a")
    return q


def _e_px_channel_extract(rng, c):
    q = _Cases()
    valid = (["r", "g", "b"] if c.rgb else []) + (["a"] if c.alpha else [])
    if valid:
        q.ok("px-channel-extract", "--channel", rng.choice(valid), *_sel(rng, c),
             note="ordinary")
        q.ok("px-channel-extract", "--channel", valid[0], "--all",
             note="boundary: every raster record")
        q.ok("px-channel-extract", "--channel", valid[-1], "--index",
             _pick(rng, c.rasters, 0), note="boundary: last valid channel")
    else:
        q.err("px-channel-extract", "--channel", "r", "--index", _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED (E10): r/g/b need an rgb document")
        q.err("px-channel-extract", "--channel", "a", "--all",
              note="E_MODE_UNSUPPORTED: document has no alpha channel")
    q.err("px-channel-extract", "--channel", "v", "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: channel must be r, g, b or a")
    q.err("px-channel-extract", "--index", _pick(rng, c.rasters, 0),
          note="E_USAGE: missing required flag --channel")
    return q


def _e_px_channel_set(rng, c):
    q = _Cases()
    valid = (["r", "g", "b"] if c.rgb else []) + (["a"] if c.alpha else [])
    if valid:
        q.ok("px-channel-set", "--channel", rng.choice(valid), "--value", rng.randrange(256),
             *_sel(rng, c), note="ordinary")
        q.ok("px-channel-set", "--channel", valid[0], "--value", 0, "--all",
             note="boundary value=0")
        q.ok("px-channel-set", "--channel", valid[-1], "--value", 255, "--index",
             _pick(rng, c.rasters, 0), note="boundary value=255")
    else:
        q.err("px-channel-set", "--channel", "r", "--value", 10, "--index",
              _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED (E10): r/g/b need an rgb document")
    q.err("px-channel-set", "--channel", (valid[0] if valid else "r"), "--value", 256,
          "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --value out of range (0..255)")
    q.err("px-channel-set", "--channel", "v", "--value", 1, "--index",
          _pick(rng, c.rasters, 0), note="E_BAD_ARGS: channel must be r, g, b or a")
    return q


def _e_px_channel_mix(rng, c):
    q = _Cases()
    ident = "256,0,0,0,256,0,0,0,256"
    rnd = ",".join(str(rng.randrange(-256, 513)) for _ in range(9))
    if c.rgb:
        q.ok("px-channel-mix", "--matrix", rnd, "--shift", 8, *_sel(rng, c), note="ordinary")
        q.ok("px-channel-mix", "--matrix", ident, "--shift", 8, "--all",
             note="boundary: the identity matrix")
        q.ok("px-channel-mix", "--matrix", "65535,-65535,0,0,65535,-65535,-65535,0,65535",
             "--shift", 24, "--index", _pick(rng, c.rasters, 0),
             note="boundary: extreme taps, shift 24 (E03 clamp, signed rounding)")
        q.ok("px-channel-mix", "--matrix", "1,0,0,0,1,0,0,0,1", "--shift", 0,
             "--index", _pick(rng, c.rasters, 0), note="boundary: shift 0")
    else:
        q.err("px-channel-mix", "--matrix", ident, "--shift", 8, "--index",
              _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED (E10): requires an rgb document")
        q.err("px-channel-mix", "--matrix", ident, "--all",
              note="E_MODE_UNSUPPORTED (E10): requires an rgb document")
    q.err("px-channel-mix", "--matrix", "256,0,0,0,256", "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --matrix needs 9 comma-separated integers")
    q.err("px-channel-mix", "--matrix", ident, "--shift", 25, "--index",
          _pick(rng, c.rasters, 0), note="E_BAD_ARGS: --shift out of range (0..24)")
    return q


def _e_px_alpha_set(rng, c):
    q = _Cases()
    if c.alpha:
        q.ok("px-alpha-set", "--value", rng.randrange(256), *_sel(rng, c), note="ordinary")
        q.ok("px-alpha-set", "--value", 0, "--all", note="boundary: fully transparent")
        q.ok("px-alpha-set", "--value", 255, "--index", _pick(rng, c.rasters, 0),
             note="boundary: fully opaque")
    else:
        q.err("px-alpha-set", "--value", 128, "--index", _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED: document has no alpha channel")
        q.err("px-alpha-set", "--value", 0, "--all",
              note="E_MODE_UNSUPPORTED: document has no alpha channel")
    q.err("px-alpha-set", "--value", 256, "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --value out of range (0..255)")
    q.err("px-alpha-set", "--index", _pick(rng, c.rasters, 0),
          note="E_USAGE: missing required flag --value")
    return q


def _e_px_alpha_multiply(rng, c):
    q = _Cases()
    if c.alpha:
        q.ok("px-alpha-multiply", "--num", rng.randrange(0, 512), "--den", rng.randrange(1, 512),
             *_sel(rng, c), note="ordinary")
        q.ok("px-alpha-multiply", "--num", 65535, "--den", 1, "--all",
             note="boundary: clamps to 255 (E03)")
        q.ok("px-alpha-multiply", "--num", 0, "--den", 65535, "--index",
             _pick(rng, c.rasters, 0), note="boundary: multiplies to 0")
    else:
        q.err("px-alpha-multiply", "--num", 1, "--den", 2, "--index", _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED: document has no alpha channel")
        q.err("px-alpha-multiply", "--num", 1, "--all",
              note="E_MODE_UNSUPPORTED: document has no alpha channel")
    q.err("px-alpha-multiply", "--num", 1, "--den", 0, "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --den out of range (1..65535)")
    q.err("px-alpha-multiply", "--den", 2, "--index", _pick(rng, c.rasters, 0),
          note="E_USAGE: missing required flag --num")
    return q


def _alpha_only_noarg(rng, c, op, note_ok):
    q = _Cases()
    if c.alpha:
        q.ok(op, *_sel(rng, c), note=note_ok)
        q.ok(op, "--all", note="boundary: every raster record")
        q.ok(op, "--index", (max(c.rasters) if c.rasters else 0),
             note="boundary: topmost raster")
    else:
        q.err(op, "--index", _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED: document has no alpha channel")
        q.err(op, "--all", note="E_MODE_UNSUPPORTED: document has no alpha channel")
    q.err(op, "--index", c.not_raster(),
          note="E_BAD_ARGS / E_MODE_UNSUPPORTED: --index must name a raster layer")
    q.err(op, "--all", "--index", 0,
          note="E_BAD_ARGS: --all and --index are mutually exclusive")
    return q


def _e_px_premultiply(rng, c):
    return _alpha_only_noarg(rng, c, "px-premultiply", "ordinary (E02 premultiplication order)")


def _e_px_unpremultiply(rng, c):
    return _alpha_only_noarg(rng, c, "px-unpremultiply", "ordinary")


def _e_px_clamp(rng, c):
    q = _Cases()
    lo = rng.randrange(0, 128)
    hi = rng.randrange(lo, 256)
    _lut_cases(q, rng, c, "px-clamp",
               [["--lo", lo, "--hi", hi],
                ["--lo", 0, "--hi", 255],
                ["--lo", 128, "--hi", 128]],
               [(["--lo", 200, "--hi", 100, "--index", 0],
                 "E_BAD_ARGS: --lo must not exceed --hi"),
                (["--lo", 256, "--index", 0], "E_BAD_ARGS: --lo out of range (0..255)")])
    return q


# ==========================================================================
# Tier E -- geometry family
# ==========================================================================

def _orient(rng, c, op):
    q = _Cases()
    q.ok(op, note="ordinary (whole document, all records)")
    q.err(op, "--op", "rot90", note="E_USAGE: %s accepts no flags" % op)
    q.err_raw([op, "IN.ldx"], note="E_USAGE: expected 2 positionals, got 1")
    return q


def _e_px_flip_h(rng, c):
    return _orient(rng, c, "px-flip-h")


def _e_px_flip_v(rng, c):
    return _orient(rng, c, "px-flip-v")


def _e_px_rot90(rng, c):
    return _orient(rng, c, "px-rot90")


def _e_px_rot180(rng, c):
    return _orient(rng, c, "px-rot180")


def _e_px_rot270(rng, c):
    return _orient(rng, c, "px-rot270")


def _e_px_transpose(rng, c):
    return _orient(rng, c, "px-transpose")


def _e_px_scale(rng, c):
    q = _Cases()
    up = max(1, min(8, MAX_DIM // max(c.w, c.h)))
    q.ok("px-scale", "--num", rng.randrange(1, up + 1), "--den", rng.randrange(1, 5),
         note="ordinary nearest-neighbour rational scale")
    q.ok("px-scale", "--num", 1, "--den", max(c.w, c.h),
         note="boundary: shrink to (at least) one pixel")
    q.ok("px-scale", "--num", up, "--den", 1, note="boundary: maximum safe upscale")
    q.ok("px-scale", "--num", 1, "--den", 1, note="boundary: identity scale")
    q.err("px-scale", "--num", 1, "--den", 0, note="E_BAD_ARGS: --den out of range (1..16384)")
    if max(c.w, c.h) >= 2:
        q.err("px-scale", "--num", MAX_DIM, "--den", 1,
              note="E_BAD_ARGS: resulting canvas exceeds the maximum dimension")
    else:
        q.err("px-scale", "--num", MAX_DIM + 1, "--den", 1,
              note="E_BAD_ARGS: --num out of range (1..16384)")
    return q


def _e_px_pad(rng, c):
    q = _Cases()
    q.ok("px-pad", "--left", rng.randrange(0, 5), "--right", rng.randrange(0, 5),
         "--top", rng.randrange(0, 5), "--bottom", rng.randrange(0, 5),
         "--fill", _fill(rng, c.ch), note="ordinary: grow the canvas")
    if c.w >= 2:
        q.ok("px-pad", "--left", -1, "--right", 2, "--top", 0, "--bottom", 0,
             note="boundary E07: negative pad clamps, W_CROP_CLAMPED, exit 0")
    else:
        q.ok("px-pad", "--left", 1, "--right", 1, "--top", 1, "--bottom", 1,
             note="boundary: symmetric grow")
    q.ok("px-pad", "--left", 0, "--right", 0, "--top", 0, "--bottom", 0,
         note="boundary: identity pad")
    q.err("px-pad", "--left", -c.w, "--right", 0, "--top", 0, "--bottom", 0,
          note="E_BAD_RECT: padded rectangle is empty")
    q.err("px-pad", "--left", MAX_DIM + 1,
          note="E_BAD_ARGS: --left out of range (-16384..16384)")
    return q


def _e_px_translate(rng, c):
    q = _Cases()
    q.ok("px-translate", "--dx", rng.randrange(-c.w, c.w + 1),
         "--dy", rng.randrange(-c.h, c.h + 1), "--wrap", rng.randrange(2), note="ordinary")
    q.ok("px-translate", "--dx", MAX_DIM, "--dy", -MAX_DIM, "--wrap", 1,
         note="boundary: extreme shift, wrapped")
    q.ok("px-translate", "--dx", -MAX_DIM, "--dy", MAX_DIM, "--wrap", 0,
         note="boundary: extreme shift, zero-filled")
    q.err("px-translate", "--dx", 1, "--wrap", 2, note="E_BAD_ARGS: --wrap out of range (0..1)")
    q.err("px-translate", "--dx", -MAX_DIM - 1,
          note="E_BAD_ARGS: --dx out of range (-16384..16384)")
    return q


def _e_px_tile(rng, c):
    q = _Cases()
    mc = max(1, MAX_DIM // c.w)
    mr = max(1, MAX_DIM // c.h)
    q.ok("px-tile", "--cols", rng.randrange(1, min(mc, 4) + 1),
         "--rows", rng.randrange(1, min(mr, 4) + 1), note="ordinary")
    q.ok("px-tile", "--cols", 1, "--rows", 1, note="boundary: identity tiling")
    q.ok("px-tile", "--cols", mc, "--rows", 1, note="boundary: widest legal tiling")
    q.err("px-tile", "--cols", 0, "--rows", 1, note="E_BAD_ARGS: --cols out of range (1..16384)")
    q.err("px-tile", "--cols", mc + 1, "--rows", 1,
          note="E_BAD_ARGS: resulting canvas exceeds the maximum dimension")
    return q


# ==========================================================================
# Tier E -- filter family
# ==========================================================================

def _radius_op(rng, c, op, rmax, extra_ok=(), extra_err=()):
    q = _Cases()
    q.ok(op, "--radius", rng.randrange(0, rmax + 1), *_sel(rng, c), note="ordinary")
    q.ok(op, "--radius", 0, "--all", note="boundary: radius 0 is the identity")
    q.ok(op, "--radius", rmax, "--index", _pick(rng, c.rasters, 0),
         note="boundary: maximum radius")
    for args, note in extra_ok:
        q.ok(op, *args, note=note)
    q.err(op, "--radius", rmax + 1, "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --radius out of range (0..%d)" % rmax)
    q.err(op, "--radius", 1, "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    for args, note in extra_err:
        q.err(op, *args, note=note)
    return q


def _e_px_blur_box(rng, c):
    return _radius_op(rng, c, "px-blur-box", 32)


def _e_px_blur_gauss(rng, c):
    return _radius_op(rng, c, "px-blur-gauss", 8)


def _e_px_sharpen(rng, c):
    q = _Cases()
    q.ok("px-sharpen", "--amount", rng.randrange(0, 1025), *_sel(rng, c), note="ordinary")
    q.ok("px-sharpen", "--amount", 0, "--all", note="boundary: amount 0 is the identity")
    q.ok("px-sharpen", "--amount", 1024, "--index", _pick(rng, c.rasters, 0),
         note="boundary: maximum strength (E03 clamp)")
    q.ok("px-sharpen", *_sel(rng, c), note="boundary: default amount 256")
    q.err("px-sharpen", "--amount", 1025, "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --amount out of range (0..1024)")
    q.err("px-sharpen", "--amount", -1, "--all",
          note="E_BAD_ARGS: --amount out of range (0..1024)")
    return q


def _e_px_unsharp(rng, c):
    q = _Cases()
    q.ok("px-unsharp", "--radius", rng.randrange(0, 9), "--amount", rng.randrange(0, 1025),
         *_sel(rng, c), note="ordinary")
    q.ok("px-unsharp", "--radius", 0, "--amount", 0, "--all", note="boundary: identity")
    q.ok("px-unsharp", "--radius", 8, "--amount", 1024, "--index", _pick(rng, c.rasters, 0),
         note="boundary: maximum radius and strength (E04 signed rounding)")
    q.err("px-unsharp", "--radius", 9, "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --radius out of range (0..8)")
    q.err("px-unsharp", "--radius", 1, "--amount", 1025, "--all",
          note="E_BAD_ARGS: --amount out of range (0..1024)")
    return q


def _e_px_edge(rng, c):
    q = _Cases()
    q.ok("px-edge", *_sel(rng, c), note="ordinary Sobel magnitude")
    q.ok("px-edge", "--all", note="boundary: every raster record")
    q.ok("px-edge", "--index", (max(c.rasters) if c.rasters else 0),
         note="boundary: topmost raster")
    q.err("px-edge", "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    q.err("px-edge", "--radius", 1, "--all", note="E_USAGE: unknown flag --radius")
    return q


def _e_px_emboss(rng, c):
    q = _Cases()
    q.ok("px-emboss", "--dir", rng.choice(EMBOSS_DIRS), *_sel(rng, c), note="ordinary")
    q.ok("px-emboss", "--dir", EMBOSS_DIRS[0], "--all", note="boundary: first direction")
    q.ok("px-emboss", "--dir", EMBOSS_DIRS[-1], "--index", _pick(rng, c.rasters, 0),
         note="boundary: last direction")
    q.ok("px-emboss", *_sel(rng, c), note="boundary: default direction nw")
    q.err("px-emboss", "--dir", "up", "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --dir must be n, ne, e, se, s, sw, w or nw")
    q.err("px-emboss", "--dir", "n", "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    return q


def _e_px_median(rng, c):
    return _radius_op(rng, c, "px-median", 16)


def _e_px_erode(rng, c):
    return _radius_op(rng, c, "px-erode", 16)


def _e_px_dilate(rng, c):
    return _radius_op(rng, c, "px-dilate", 16)


def _e_px_noise(rng, c):
    q = _Cases()
    q.ok("px-noise", "--seed", rng.randrange(0, 2 ** 31), "--amount", rng.randrange(0, 256),
         *_sel(rng, c), note="ordinary seeded LCG noise")
    q.ok("px-noise", "--seed", 0, "--amount", 0, "--all",
         note="boundary: amount 0 is the identity, seed 0")
    q.ok("px-noise", "--seed", 2147483647, "--amount", 255, "--index",
         _pick(rng, c.rasters, 0), note="boundary: maximum seed and amount (E03 clamp)")
    q.err("px-noise", "--seed", 2147483648, "--amount", 8, "--all",
          note="E_BAD_ARGS: --seed out of range (0..2147483647)")
    q.err("px-noise", "--seed", 1, "--amount", 256, "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --amount out of range (0..255)")
    return q


def _e_px_dither(rng, c):
    q = _Cases()
    q.ok("px-dither", "--levels", rng.randrange(2, 65), *_sel(rng, c), note="ordinary Bayer 4x4")
    q.ok("px-dither", "--levels", 2, "--all", note="boundary: two levels")
    q.ok("px-dither", "--levels", 256, "--index", _pick(rng, c.rasters, 0),
         note="boundary: 256 levels")
    q.err("px-dither", "--levels", 1, "--all", note="E_BAD_ARGS: --levels out of range (2..256)")
    q.err("px-dither", "--levels", 257, "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --levels out of range (2..256)")
    return q


# ==========================================================================
# Tier E -- document, selection and statistics families
# ==========================================================================

def _e_doc_info(rng, c):
    q = _Cases()
    q.ok_r("doc-info", note="ordinary: all three sections")
    q.ok_r("doc-info", "--fields", rng.choice(("document", "counts", "layers")),
           note="boundary: a single section")
    q.ok_r("doc-info", "--fields", "document,counts,layers",
           note="boundary: every section named explicitly")
    q.err_r("doc-info", "--fields", "pixels",
            note="E_BAD_ARGS: --fields items must be document, counts or layers")
    q.err_r("doc-info", "--index", 0, note="E_USAGE: unknown flag --index")
    return q


def _e_doc_resize(rng, c):
    q = _Cases()
    q.ok("doc-resize", "--w", rng.randrange(1, c.w + 5), "--h", rng.randrange(1, c.h + 5),
         "--anchor", rng.choice(ANCHORS), "--fill", _fill(rng, c.ch), note="ordinary")
    q.ok("doc-resize", "--w", 1, "--h", 1, "--anchor", "cc",
         note="boundary: shrink to 1x1 (E07 clamp, exit 0)")
    q.ok("doc-resize", "--w", c.w + 3, "--h", c.h + 2, "--anchor", "br",
         "--fill", ",".join(["0"] * c.ch), note="boundary: grow from the bottom right")
    q.err("doc-resize", "--w", 0, "--h", c.h,
          note="E_BAD_RECT: resize would produce an empty canvas")
    q.err("doc-resize", "--w", c.w, "--h", c.h, "--anchor", "middle",
          note="E_BAD_ARGS: --anchor must be tl..br")
    q.err("doc-resize", "--w", MAX_DIM + 1, "--h", c.h,
          note="E_BAD_ARGS: --w out of range (0..16384)")
    return q


def _e_doc_convert(rng, c):
    q = _Cases()
    q.ok("doc-convert", "--mode", "gray" if c.rgb else "rgb",
         note="ordinary: change colour model")
    q.ok("doc-convert", "--alpha", 1 - c.alpha, note="boundary: toggle the alpha channel")
    q.ok("doc-convert", "--mode", c.mode, "--alpha", c.alpha,
         note="boundary: no-op rewrite")
    q.err("doc-convert", "--mode", "cmyk", note="E_BAD_ARGS: --mode must be gray or rgb")
    q.err("doc-convert", "--mode", "indexed",
          note="E_MODE_UNSUPPORTED: indexed mode not supported")
    q.err("doc-convert", "--alpha", 2, note="E_BAD_ARGS: --alpha out of range (0..1)")
    return q


def _e_doc_trim(rng, c):
    q = _Cases()
    q.ok("doc-trim", note="ordinary: drop a fully transparent border")
    q.err("doc-trim", "--index", 0, note="E_USAGE: doc-trim accepts no flags")
    q.err_raw(["doc-trim", "IN.ldx"], note="E_USAGE: expected 2 positionals, got 1")
    return q


def _e_doc_set_dpi(rng, c):
    q = _Cases()
    q.ok("doc-set-dpi", "--dpi", rng.randrange(1, 1201), note="ordinary")
    q.ok("doc-set-dpi", "--dpi", 1, note="boundary: minimum dpi")
    q.ok("doc-set-dpi", "--dpi", 65535, note="boundary: maximum dpi")
    q.err("doc-set-dpi", "--dpi", 0, note="E_BAD_ARGS: --dpi out of range (1..65535)")
    q.err("doc-set-dpi", "--dpi", 65536, note="E_BAD_ARGS: --dpi out of range (1..65535)")
    return q


def _e_doc_diff(rng, c):
    q = _Cases()
    q.ok_raw(["doc-diff", "IN.ldx", "IN.ldx"], note="ordinary: a document against itself")
    if c.file:
        q.ok_raw(["doc-diff", "IN.ldx", "B.ldx"], {"B.ldx": c.file},
                 note="ordinary: against a copy supplied separately")
    q.err_raw(["doc-diff", "IN.ldx"], note="E_USAGE: expected 2 positionals, got 1")
    q.err_raw(["doc-diff", "--fields", "document", "IN.ldx", "IN.ldx"],
              note="E_USAGE: doc-diff accepts no flags")
    return q


def _e_sel_rect(rng, c):
    q = _Cases()
    idx = _pick(rng, c.rasters, 0)
    x = rng.randrange(0, c.w)
    y = rng.randrange(0, c.h)
    q.ok("sel-rect", "--x", x, "--y", y, "--w", rng.randrange(1, c.w - x + 1),
         "--h", rng.randrange(1, c.h - y + 1), "--index", idx, note="ordinary")
    q.ok("sel-rect", "--x", 0, "--y", 0, "--w", c.w, "--h", c.h, "--index", idx,
         note="boundary: select the whole canvas")
    q.ok("sel-rect", "--x", -1, "--y", -1, "--w", c.w + 2, "--h", c.h + 2, "--index", idx,
         note="boundary E07: clamped, W_CROP_CLAMPED, exit 0")
    q.err("sel-rect", "--x", c.w, "--y", 0, "--w", 1, "--h", 1, "--index", idx,
          note="E_BAD_RECT: rectangle does not intersect the canvas")
    q.err("sel-rect", "--x", 0, "--y", 0, "--w", 0, "--h", 1, "--index", idx,
          note="E_BAD_ARGS: --w out of range (1..16384)")
    q.err("sel-rect", "--x", 0, "--y", 0, "--w", 1, "--h", 1, "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    return q


def _e_sel_from_alpha(rng, c):
    q = _Cases()
    if c.alpha:
        q.ok("sel-from-alpha", "--index", _pick(rng, c.rasters, 0), note="ordinary")
        if c.masked:
            q.ok("sel-from-alpha", "--index", _pick(rng, c.masked),
                 note="boundary: overwrite an existing mask record")
        if c.unmasked:
            q.ok("sel-from-alpha", "--index", _pick(rng, c.unmasked),
                 note="boundary: create the mask record")
    else:
        q.err("sel-from-alpha", "--index", _pick(rng, c.rasters, 0),
              note="E_MODE_UNSUPPORTED: document has no alpha channel")
    q.err("sel-from-alpha", "--index", c.not_raster(),
          note="E_BAD_ARGS: --index must be the index of a raster layer")
    q.err("sel-from-alpha", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    return q


def _e_sel_invert(rng, c):
    q = _Cases()
    if c.masked:
        q.ok("sel-invert", "--index", _pick(rng, c.masked), note="ordinary: via the raster")
    if c.masks:
        q.ok("sel-invert", "--index", _pick(rng, c.masks),
             note="boundary: naming the mask record itself")
    if c.unmasked:
        q.err("sel-invert", "--index", _pick(rng, c.unmasked),
              note="E_BAD_ARGS: layer has no mask record to invert")
    q.err("sel-invert", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    if c.groups or c.closes:
        q.err("sel-invert", "--index", (c.groups + c.closes)[0],
              note="E_BAD_ARGS: --index must be a raster or mask record")
    return q


def _e_sel_apply(rng, c):
    q = _Cases()
    if c.masked:
        i = _pick(rng, c.masked)
        q.ok("sel-apply", "--op", "clear", "--index", i, note="ordinary clear")
        q.ok("sel-apply", "--op", "fill", "--colour", _fill(rng, c.ch), "--index", i,
             note="ordinary fill")
        q.ok("sel-apply", "--op", "fill", "--colour", ",".join(["255"] * c.ch), "--index", i,
             note="boundary: fill with every channel at 255")
        q.err("sel-apply", "--op", "clear", "--colour", _fill(rng, c.ch), "--index", i,
              note="E_BAD_ARGS: --colour is only valid with --op fill")
        q.err("sel-apply", "--op", "fill", "--index", i,
              note="E_USAGE: missing required flag --colour")
    else:
        q.err("sel-apply", "--op", "clear", "--index", _pick(rng, c.rasters, 0),
              note="E_BAD_ARGS: layer has no mask record to apply")
        q.err("sel-apply", "--op", "fill", "--colour", _fill(rng, c.ch),
              "--index", _pick(rng, c.rasters, 0),
              note="E_BAD_ARGS: layer has no mask record to apply")
    q.err("sel-apply", "--op", "erase", "--index", _pick(rng, c.rasters, 0),
          note="E_BAD_ARGS: --op must be clear or fill")
    return q


def _e_stat_histogram(rng, c):
    q = _Cases()
    idx = _pick(rng, c.rasters, 0)
    q.ok_r("stat-histogram", "--index", idx, note="ordinary: default channel")
    if c.rgb:
        q.ok_r("stat-histogram", "--index", idx, "--channel", rng.choice(("r", "g", "b")),
               note="boundary: a named colour channel")
    else:
        q.ok_r("stat-histogram", "--index", idx, "--channel", "v",
               note="boundary: the gray channel")
    if c.alpha:
        q.ok_r("stat-histogram", "--index", idx, "--channel", "a",
               note="boundary: the alpha channel")
    elif c.masks:
        q.ok_r("stat-histogram", "--index", _pick(rng, c.masks), "--channel", "v",
               note="boundary: a mask record")
    q.err_r("stat-histogram", "--index", idx, "--channel", "z",
            note="E_BAD_ARGS: --channel must be r, g, b, a or v")
    q.err_r("stat-histogram", "--index", idx, "--channel", ("v" if c.rgb else "r"),
            note="E_BAD_ARGS / E_MODE_UNSUPPORTED (E10): channel does not exist in this mode")
    return q


def _e_stat_bbox(rng, c):
    q = _Cases()
    q.ok_r("stat-bbox", "--index", _pick(rng, c.rasters, 0), note="ordinary")
    q.ok_r("stat-bbox", "--index", (max(c.rasters) if c.rasters else 0),
           note="boundary: topmost raster")
    if c.masks:
        q.ok_r("stat-bbox", "--index", _pick(rng, c.masks), note="boundary: a mask record")
    q.err_r("stat-bbox", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    if c.groups or c.closes:
        q.err_r("stat-bbox", "--index", (c.groups + c.closes)[0],
                note="E_BAD_ARGS: --index must be a raster or mask record")
    return q


def _e_stat_checksum(rng, c):
    q = _Cases()
    q.ok_r("stat-checksum", "--index", rng.randrange(0, c.n) if c.n else 0, note="ordinary")
    q.ok_r("stat-checksum", "--index", 0, note="boundary: the bottom record")
    q.ok_r("stat-checksum", "--index", max(c.n - 1, 0), note="boundary: the top record")
    if c.groups or c.closes:
        q.ok_r("stat-checksum", "--index", (c.groups + c.closes)[0],
               note="boundary: a group marker digests the bare FNV basis")
    q.err_r("stat-checksum", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    q.err_r("stat-checksum", "--index", MAX_LAYERS + 1,
            note="E_BAD_ARGS: --index out of range (0..4096)")
    return q


def _e_stat_count(rng, c):
    q = _Cases()
    q.ok_r("stat-count", "--index", _pick(rng, c.rasters, 0), note="ordinary")
    q.ok_r("stat-count", "--index", (max(c.rasters) if c.rasters else 0),
           note="boundary: topmost raster")
    if c.masks:
        q.ok_r("stat-count", "--index", _pick(rng, c.masks), note="boundary: a mask record")
    q.err_r("stat-count", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    if c.groups or c.closes:
        q.err_r("stat-count", "--index", (c.groups + c.closes)[0],
                note="E_BAD_ARGS: --index must be a raster or mask record")
    return q


# --- X1: the container-codec family ---------------------------------------
# Record payloads may be stored compressed; the codec id lives in flags bits 4-7 and
# `data_len` on disk is the STORED length.  The hidden documents are all codec 0, so
# what these cases grade is the ENCODER: `doc-recompress` and `doc-save --compress`
# write the compressed bytes the comparison then reads back.

def _e_doc_recompress(rng, c):
    q = _Cases()
    q.ok("doc-recompress", "--compress", "auto",
         note="ordinary E19: smallest encoding per record, ties to the lower codec id")
    q.ok("doc-recompress", "--compress", "rle", "--index", _pick(rng, c.rasters, 0),
         note="boundary E17/E18: one record, RLE, runs restarting at every row")
    q.ok("doc-recompress", "--compress", "packbits",
         note="boundary E17/E20: PackBits over the whole stack")
    q.ok("doc-recompress", "--compress", "delta", "--index", -1,
         note="boundary: --index -1 is every record; delta filters per pixel, then RLE")
    q.ok("doc-recompress", "--compress", "none", "--index", max(c.n - 1, 0),
         note="boundary: the top record back to stored-raw")
    if rng.randrange(2):
        q.err("doc-recompress", "--compress", BAD_CODEC,
              note="E_BAD_ARGS: --compress must be none, rle, packbits, delta or auto")
    else:
        q.err("doc-recompress", "--compress", "auto", "--index", c.bad_index(),
              note="E_BAD_ARGS: --index out of range")
    return q


def _e_doc_repack(rng, c):
    q = _Cases()
    q.ok("doc-repack", note="ordinary: re-lay every record, codecs untouched")
    q.ok("doc-repack", "--compress", rng.choice(CODECS + ("auto",)),
         note="boundary: --compress is accepted and ignored -- repack never re-codes")
    q.err("doc-repack", "--index", 0, note="E_USAGE: unknown flag --index")
    q.err_raw(["doc-repack", "IN.ldx"], note="E_USAGE: expected 2 positionals, got 1")
    return q


def _e_doc_stat_size(rng, c):
    q = _Cases()
    q.ok_r("doc-stat-size", note="ordinary: every record, raw and stored length")
    q.ok_r("doc-stat-size", "--index", _pick(rng, c.rasters, 0), note="boundary: one raster")
    q.ok_r("doc-stat-size", "--index", -1, note="boundary: --index -1 is every record")
    q.ok_r("doc-stat-size", "--index", max(c.n - 1, 0),
           note="boundary: the top record (a group marker stores nothing)")
    q.err_r("doc-stat-size", "--index", c.bad_index(), note="E_BAD_ARGS: --index out of range")
    q.err_r("doc-stat-size", "--index", MAX_INDEX + 1,
            note="E_BAD_ARGS: --index out of range (-1..65535)")
    return q


def _e_doc_verify(rng, c):
    q = _Cases()
    q.ok_r("doc-verify", note="ordinary: record count and codec faults as JSON")
    q.ok_r("doc-verify", "--strict",
           note="boundary: --strict takes no value; a canonical file still exits 0")
    q.err_r("doc-verify", "--index", 0, note="E_USAGE: unknown flag --index")
    q.err_raw(["doc-verify"], note="E_USAGE: expected 1 positional, got 0")
    q.err_raw(["doc-verify", "--strict", "IN.ldx", "IN.ldx"],
              note="E_USAGE: expected 1 positional, got 2")
    return q


# --------------------------------------------------------------------------
# Dispatch table
# --------------------------------------------------------------------------

_GEN = {
    # Tier C
    "doc-new": _c_doc_new,
    "doc-open": _c_doc_open,
    "doc-save": _c_doc_save,
    "layer-add": _c_layer_add,
    "layer-set": _c_layer_set,
    "layer-reorder": _c_layer_reorder,
    "layer-merge-down": _c_layer_merge_down,
    "layer-mask-apply": _c_layer_mask_apply,
    "doc-flatten": _c_doc_flatten,
    "px-crop": _c_px_crop,
    "px-transform": _c_px_transform,
    "px-channel": _c_px_channel,
    "px-convolve": _c_px_convolve,
    # Tier G
    "gen-fill": _g_gen_fill,
    "gen-scale": _g_gen_scale,
    "gen-heal": _g_gen_heal,
    "gen-extend": _g_gen_extend,
    "gen-retarget": _g_gen_retarget,
    "gen-denoise": _g_gen_denoise,
    # Tier E
    "layer-delete": _e_layer_delete,
    "layer-duplicate": _e_layer_duplicate,
    "layer-rename": _e_layer_rename,
    "layer-group": _e_layer_group,
    "layer-ungroup": _e_layer_ungroup,
    "layer-merge-visible": _e_layer_merge_visible,
    "layer-mask-add": _e_layer_mask_add,
    "layer-mask-remove": _e_layer_mask_remove,
    "layer-mask-invert": _e_layer_mask_invert,
    "layer-fill": _e_layer_fill,
    "layer-clear": _e_layer_clear,
    "layer-offset": _e_layer_offset,
    "layer-copy": _e_layer_copy,
    "layer-swap": _e_layer_swap,
    "layer-opacity-scale": _e_layer_opacity_scale,
    "layer-blend-set": _e_layer_blend_set,
    "layer-lock-all": _e_layer_lock_all,
    "px-invert": _e_px_invert,
    "px-threshold": _e_px_threshold,
    "px-offset": _e_px_offset,
    "px-scale-value": _e_px_scale_value,
    "px-gamma": _e_px_gamma,
    "px-brightness": _e_px_brightness,
    "px-contrast": _e_px_contrast,
    "px-levels": _e_px_levels,
    "px-posterize": _e_px_posterize,
    "px-solarize": _e_px_solarize,
    "px-desaturate": _e_px_desaturate,
    "px-channel-swap": _e_px_channel_swap,
    "px-channel-extract": _e_px_channel_extract,
    "px-channel-set": _e_px_channel_set,
    "px-channel-mix": _e_px_channel_mix,
    "px-alpha-set": _e_px_alpha_set,
    "px-alpha-multiply": _e_px_alpha_multiply,
    "px-premultiply": _e_px_premultiply,
    "px-unpremultiply": _e_px_unpremultiply,
    "px-clamp": _e_px_clamp,
    "px-flip-h": _e_px_flip_h,
    "px-flip-v": _e_px_flip_v,
    "px-rot90": _e_px_rot90,
    "px-rot180": _e_px_rot180,
    "px-rot270": _e_px_rot270,
    "px-transpose": _e_px_transpose,
    "px-scale": _e_px_scale,
    "px-pad": _e_px_pad,
    "px-translate": _e_px_translate,
    "px-tile": _e_px_tile,
    "px-blur-box": _e_px_blur_box,
    "px-blur-gauss": _e_px_blur_gauss,
    "px-sharpen": _e_px_sharpen,
    "px-unsharp": _e_px_unsharp,
    "px-edge": _e_px_edge,
    "px-emboss": _e_px_emboss,
    "px-median": _e_px_median,
    "px-erode": _e_px_erode,
    "px-dilate": _e_px_dilate,
    "px-noise": _e_px_noise,
    "px-dither": _e_px_dither,
    "doc-info": _e_doc_info,
    "doc-resize": _e_doc_resize,
    "doc-convert": _e_doc_convert,
    "doc-trim": _e_doc_trim,
    "doc-set-dpi": _e_doc_set_dpi,
    "doc-diff": _e_doc_diff,
    "sel-rect": _e_sel_rect,
    "sel-from-alpha": _e_sel_from_alpha,
    "sel-invert": _e_sel_invert,
    "sel-apply": _e_sel_apply,
    "stat-histogram": _e_stat_histogram,
    "stat-bbox": _e_stat_bbox,
    "stat-checksum": _e_stat_checksum,
    "stat-count": _e_stat_count,
    "doc-recompress": _e_doc_recompress,
    "doc-repack": _e_doc_repack,
    "doc-stat-size": _e_doc_stat_size,
    "doc-verify": _e_doc_verify,
}

assert set(_GEN) == set(ALL_OPS), "generator table and ALL_OPS disagree"


def annotated_cases(op, rng, ctx):
    """cases() plus the bookkeeping the self-test needs.

    Returns a list of (argv, extra_inputs, expect, note) where `expect` is "ok" when
    the invocation must exit 0 and "err" when it must exit non-zero with the error
    code named in `note`.  The verifier only needs `cases()`; this is exported so the
    grid can be self-checked against the reference binary.
    """
    if op not in _GEN:
        raise KeyError("unknown operation: %r" % (op,))
    if rng is None:
        rng = random.Random(0)
    items = _GEN[op](rng, _Ctx(ctx))
    return [(list(argv), dict(extra), expect, note)
            for argv, extra, expect, note in items.items]


def cases(op, rng, ctx):
    """Return canonical invocations for one operation.

    Each element is (argv, extra_inputs); see OPSPEC_INTERFACE.md.
    """
    return [(argv, extra) for argv, extra, _e, _n in annotated_cases(op, rng, ctx)]


# ==========================================================================
# Self-test
# ==========================================================================

def _pnm(path, w, h, ch, seed=0):
    rnd = random.Random(seed)
    body = bytes(rnd.randrange(256) for _ in range(w * h * ch))
    head = (b"P6" if ch == 3 else b"P5") + ("\n%d %d\n255\n" % (w, h)).encode()
    with open(path, "wb") as fh:
        fh.write(head + body)
    return path


def _selftest(repeats=1):  # pragma: no cover - developer tool
    import json
    import shutil
    import subprocess
    import tempfile

    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(os.path.dirname(here), "build", "refldx-src")
    binary = None
    if os.path.isdir(src):
        binary = os.path.join(tempfile.gettempdir(), "ldx_opspec_selftest")
        rc = subprocess.call(["cc", "-O2", "-std=c99", "-o", binary, "ldx.c"], cwd=src)
        if rc != 0:
            binary = None
    if binary is None:
        cand = os.path.join(here, "refldx")
        binary = cand if os.path.exists(cand) else None
    if binary is None:
        print("self-test: no reference binary available")
        return 1

    root = tempfile.mkdtemp(prefix="opspec-")

    def run(argv, cwd):
        return subprocess.run([binary] + [str(a) for a in argv], cwd=cwd,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # ------------------------------------------------------------------
    # Fixture documents.  "deep" gives the stack
    #   0 raster (background)  1 raster  2 mask  3 group_open  4 raster  5 group_close
    # so every structural case in the grid has something to bite on; "flat" has no
    # group and no mask; "solo" is a single-record document.
    # ------------------------------------------------------------------
    shapes = [
        ("deep", "rgb", 1, 9, 7), ("deep", "rgb", 0, 9, 7),
        ("deep", "gray", 1, 9, 7), ("deep", "gray", 0, 9, 7),
        ("flat", "rgb", 1, 5, 4), ("flat", "gray", 0, 20, 3),
        ("solo", "gray", 0, 1, 1), ("solo", "rgb", 1, 1, 1),
        ("solo", "rgb", 1, 12, 12),
    ]
    fixtures = []
    for n, (shape, mode, alpha, w, h) in enumerate(shapes):
        cc = 3 if mode == "rgb" else 1
        opaque = ",".join(["120"] * cc + (["255"] if alpha else []))
        half = ",".join(["60"] * cc + (["128"] if alpha else []))
        d = os.path.join(root, "fx%02d_%s_%s_a%d" % (n, shape, mode, alpha))
        os.makedirs(d)
        steps = [["doc-new", "--w", w, "--h", h, "--mode", mode, "--alpha", alpha,
                  "--dpi", 72, "--fill", opaque, "doc.ldx"]]
        if shape == "deep":
            steps += [
                ["layer-add", "--name", "Mid", "--fill", half, "doc.ldx", "doc.ldx"],
                ["layer-mask-add", "--index", 1, "--fill", 200, "doc.ldx", "doc.ldx"],
                ["layer-add", "--name", "Grp", "--kind", "group", "doc.ldx", "doc.ldx"],
                ["layer-add", "--name", "Inner", "--parent", 3, "--fill", opaque,
                 "doc.ldx", "doc.ldx"],
            ]
        elif shape == "flat":
            steps += [
                ["layer-add", "--name", "A", "--fill", half, "doc.ldx", "doc.ldx"],
                ["layer-add", "--name", "B", "--fill", opaque, "doc.ldx", "doc.ldx"],
            ]
        bad = False
        for s in steps:
            r = run(s, d)
            if r.returncode != 0:
                print("fixture step failed: %s -> %s" % (s, r.stderr.decode()[:200]))
                bad = True
                break
        if bad:
            continue
        info = json.loads(run(["doc-info", "doc.ldx"], d).stdout.decode())
        fixtures.append({
            "w": w, "h": h, "mode": mode, "alpha": alpha, "channels": cc + alpha,
            "pnm": _pnm(os.path.join(d, "img.pnm"), w, h, cc, seed=n),
            "alpha_pnm": _pnm(os.path.join(d, "img_alpha.pgm"), w, h, 1, seed=100 + n),
            "file": os.path.join(d, "doc.ldx"),
            "layers": [{"index": L["index"], "kind": L["kind"], "parent": L["parent"]}
                       for L in info["layers"]],
        })

    known = set()
    hr = subprocess.run([binary, "help"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for line in hr.stdout.decode().splitlines()[2:]:
        if line.strip():
            known.add(line.strip())

    tally = {"ok": 0, "err": 0, "missing": 0}
    unexpected = []
    per_tier = {"core": 0, "gen": 0, "ext": 0}
    total = 0
    first_pass = None

    for rep in range(max(1, int(repeats))):
        rng = random.Random(20260909 + rep)
        for ctx in fixtures:
            for op in ALL_OPS:
                items = annotated_cases(op, rng, ctx)
                if not (2 <= len(items) <= 6):
                    unexpected.append((op, "case count %d outside 2..6" % len(items), "", ""))
                for argv, extra, expect, note in items:
                    total += 1
                    per_tier[tier_of(op)] += 1
                    if op not in known:
                        tally["missing"] += 1
                        continue
                    wd = tempfile.mkdtemp(dir=root)
                    shutil.copyfile(ctx["file"], os.path.join(wd, "IN.ldx"))
                    for name, path in extra.items():
                        shutil.copyfile(path, os.path.join(wd, name))
                    r = run(argv, wd)
                    shutil.rmtree(wd, ignore_errors=True)
                    if expect == OK and r.returncode == 0:
                        tally["ok"] += 1
                    elif expect == ERR and r.returncode != 0:
                        tally["err"] += 1
                    else:
                        unexpected.append((op, note, " ".join(str(a) for a in argv),
                                           "rc=%d %s" % (r.returncode,
                                                         r.stderr.decode().strip()[:120])))
        if first_pass is None:
            first_pass = (total, dict(per_tier))

    shutil.rmtree(root, ignore_errors=True)

    print("tiers: core=%d gen=%d ext=%d  (ALL_OPS=%d, `refldx help` lists %d)"
          % (len(TIER_CORE), len(TIER_GEN), len(TIER_EXT), len(ALL_OPS), len(known)))
    print("one full pass over %d fixture contexts: %d cases (core=%d gen=%d ext=%d)"
          % (len(fixtures), first_pass[0], first_pass[1]["core"], first_pass[1]["gen"],
             first_pass[1]["ext"]))
    print("ran %d passes, %d cases total" % (max(1, int(repeats)), total))
    print("exited 0 as intended : %d" % tally["ok"])
    print("errored as intended  : %d" % tally["err"])
    print("command not in binary: %d  (%s)"
          % (tally["missing"],
             ", ".join(o for o in ALL_OPS if o not in known) or "none"))
    print("unexpected failures  : %d" % len(unexpected))
    for op, note, argv, why in unexpected[:40]:
        print("  %-20s %s\n      %s\n      %s" % (op, note, argv, why))
    return 0 if not unexpected else 1


if __name__ == "__main__":
    import sys
    raise SystemExit(_selftest(int(sys.argv[1]) if len(sys.argv) > 1 else 1))
