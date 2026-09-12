"""tests/genprop.py -- property checks for the generative tier (gen-fill, gen-scale,
gen-heal, gen-extend, gen-retarget, gen-denoise).

These six commands are searches.  Their exact output cannot be specified without handing
over the implementation, so `SPEC.md` states an OBJECTIVE and a CONTRACT instead of an
algorithm, and this module checks the contract rather than comparing bytes:

    structure      the output re-parses, the canvas changes exactly as the flags demand,
                   every record's metadata survives, padding and reserved bytes stay zero
                   (that part is `ldxfmt.structural_diff` with the payload comparison off)
    discipline     pixels outside the target region, and every record that is not the
                   target, are bit-identical to the input
    provenance     synthesised content is COPIED from content that was already there --
                   gen-fill from a legal source patch, gen-scale's and gen-retarget's
                   survivors from the row or column they came from, gen-extend's new lines
                   from a legal source line -- and the documented exceptions, the seam
                   commands' inserted pixel and gen-denoise's weighted average, stay inside
                   the range of the content they were averaged from
    objective      the quantity each command is supposed to minimise is bounded.  Where the
                   optimum is computable the bound is absolute (gen-scale's and
                   gen-retarget's seam energy are compared against a dynamic program this
                   module runs itself); elsewhere it is a stated factor of the value the
                   reference achieves on the same input.

The reference therefore enters only through the relative bounds, never through byte
equality, and every bound holds for the reference by construction.

Nothing here imports anything outside the standard library and `ldxfmt`.
"""

import struct

import ldxfmt

# Objective bounds.  K multiplies the reference's achieved value; TOL is an absolute
# allowance per compared sample, in squared-difference units, so TOL = 32 forgives a root
# mean square difference of about 5.7 per channel before a case can fail.
FILL_K, FILL_TOL = 3.0, 8.0       # patch coherence of the filled region (a ceiling)
FILL_TV_K, FILL_TV_TOL = 0.25, 64.0   # variation inside the filled region (a floor)
HEAL_K, HEAL_TOL = 1.5, 8.0       # discontinuity across and inside the relaxed region
SCALE_K, SCALE_TOL = 2.0, 16.0    # energy of the removed columns, multi-seam case
RETARGET_K, RETARGET_TOL = 2.0, 16.0  # energy of the removed rows, multi-seam case
EXT_K, EXT_TOL = 2.0, 4.0         # continuation cost of the synthesised strip
DENOISE_K, DENOISE_TOL = 1.5, 8.0 # roughness of the denoised layer (a ceiling)
FILL_BUDGET = 40000              # (region x source) pairs above which coherence is skipped
PURITY_BUDGET = 200000           # (pixels x neighbourhood) above which purity is skipped

LUMA = (77, 150, 29)


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------

def flags(argv):
    """`["gen-fill", "--seed", 5, ...]` -> `{"seed": "5", ...}`."""
    out = {}
    toks = [str(t) for t in argv]
    i = 1
    while i < len(toks):
        if toks[i].startswith("--"):
            name = toks[i][2:]
            out[name] = toks[i + 1] if i + 1 < len(toks) else ""
            i += 2
        else:
            i += 1
    return out


def _int(fl, name, default=None):
    try:
        return int(fl[name])
    except (KeyError, ValueError):
        return default


def doc(data):
    """Parse into the shape the checks want, or None if the header is unreadable."""
    p = ldxfmt.parse(data)
    hdr = p["header"]
    if hdr is None:
        return None
    cc = 3 if hdr["mode"] == 1 else 1
    ch = cc + (1 if hdr["flags"] & 1 else 0)
    return {"w": hdr["width"], "h": hdr["height"], "cc": cc, "ch": ch, "layers": p["layers"]}


def pix_records(d):
    """(record index, bytes per pixel) for every record that carries pixels."""
    out = []
    for i, L in enumerate(d["layers"]):
        if L["kind"] == 0:
            out.append((i, d["ch"]))
        elif L["kind"] == 3:
            out.append((i, 1))
    return out


def _same_shape(a, b):
    return (a is not None and b is not None and len(a["layers"]) == len(b["layers"])
            and all(x["kind"] == y["kind"] for x, y in zip(a["layers"], b["layers"])))


def region(d, fl, idx):
    """The synthesis region as a 0/1 bytearray, or (None, why).  Mirrors SPEC 11.2."""
    w, h = d["w"], d["h"]
    reg = bytearray(w * h)
    if all(k in fl for k in ("x", "y", "w", "h")):
        x, y = _int(fl, "x", 0), _int(fl, "y", 0)
        rw, rh = _int(fl, "w", 0), _int(fl, "h", 0)
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(w, x + rw), min(h, y + rh)
        if x1 <= x0 or y1 <= y0:
            return None, "region rectangle does not intersect the canvas"
        for r in range(y0, y1):
            for c in range(x0, x1):
                reg[r * w + c] = 1
        return reg, ""
    if idx + 1 >= len(d["layers"]) or d["layers"][idx + 1]["kind"] != 3:
        return None, "layer has no mask record"
    mask = d["layers"][idx + 1]["data"]
    if len(mask) < w * h:
        return None, "mask record is short"
    for p in range(w * h):
        reg[p] = 1 if mask[p] else 0
    return reg, ""


def _rows(d, w, h):
    """Per-row lists of per-column keys, each key concatenating every pixel-bearing record.

    Two columns share a key only when every record agrees on them, so a check written
    against these keys enforces that the layers, masks included, moved in lockstep.
    """
    recs = [(d["layers"][i]["data"], bpp) for i, bpp in pix_records(d)]
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            key = []
            for data, bpp in recs:
                o = (y * w + x) * bpp
                key.append(bytes(data[o:o + bpp]))
            row.append(b"".join(key))
        out.append(row)
    return out


def _cols(d, w, h):
    """`_rows` transposed: per-column lists of per-row keys.

    gen-retarget is gen-scale reflected through the diagonal, so every structural check it
    needs is the row check applied to these.  As with `_rows`, two rows share a key only
    when every pixel-bearing record agrees on them, which is what enforces lockstep.
    """
    recs = [(d["layers"][i]["data"], bpp) for i, bpp in pix_records(d)]
    out = []
    for x in range(w):
        col = []
        for y in range(h):
            key = []
            for data, bpp in recs:
                o = (y * w + x) * bpp
                key.append(bytes(data[o:o + bpp]))
            col.append(b"".join(key))
        out.append(col)
    return out


def _cut_candidates(long_row, short_row):
    """Indices of `long_row` whose removal yields `short_row`, as an inclusive range.

    Serves both directions: for a removed seam `long_row` is the input row, and for an
    inserted seam it is the output row and the index is the column the pixel was spliced at.
    Returns (lo, hi) with lo > hi when no single removal works.
    """
    n = len(long_row)
    if n != len(short_row) + 1:
        return 1, 0
    pre = 0
    while pre < len(short_row) and long_row[pre] == short_row[pre]:
        pre += 1
    suf = 0
    while suf < len(short_row) and long_row[n - 1 - suf] == short_row[len(short_row) - 1 - suf]:
        suf += 1
    return max(0, n - 1 - suf), pre


# --------------------------------------------------------------------------
# derived probe inputs
# --------------------------------------------------------------------------

def rebuild(data, payloads):
    """Re-emit a document with some record payloads replaced.  `payloads` maps record index
    to new bytes of the same length.  Everything else -- header, names, metadata, zero-fill --
    is carried through exactly, so the result is a legal input for any command.
    """
    p = ldxfmt.parse(data)
    hdr = p["header"]
    out = bytearray(struct.pack("<4sHHIIBBHI", hdr["magic"], hdr["version"], hdr["flags"],
                                hdr["width"], hdr["height"], hdr["mode"], hdr["bit_depth"],
                                len(p["layers"]), hdr["resolution"]))
    out += b"\0" * 8
    for i, L in enumerate(p["layers"]):
        name = L["name"]
        out.append(len(name))
        out += name
        out += b"\0" * ((4 - (1 + len(name)) % 4) % 4)
        body = bytes(payloads.get(i, L["data"]))
        out += struct.pack("<BBBBhHI", L["kind"], L["opacity"], L["blend"], L["flags"],
                           L["parent"], 0, len(body))
        out += body
        out += b"\0" * ((4 - len(body) % 4) % 4)
    return bytes(out)


def texturize(data):
    """The same document with every raster payload replaced by a deterministic texture.

    The hidden documents are mostly flat, and a flat document cannot tell a real search from
    a lazy one: every seam has the same energy, every patch matches every other patch and
    leaving a region alone is already smooth.  The graded probe therefore runs each command a
    second time on this derived input, where the objectives have something to bite on.
    Structure, dimensions and metadata are untouched, so the same invocation applies.

    The texture is a 5x3 tile of pseudo-random colours.  Tiling, rather than noise, is what
    makes every objective meaningful at once: patches repeat, so a real fill can score near
    zero and a lazy one cannot; the tile edges are sharp, so a region left alone is measurably
    rougher than one that was relaxed; and the content genuinely continues, so a strip that
    keeps scanning beats a strip that repeats the edge line.
    """
    p = ldxfmt.parse(data)
    hdr = p["header"]
    if hdr is None:
        return None
    cc = 3 if hdr["mode"] == 1 else 1
    ch = cc + (1 if hdr["flags"] & 1 else 0)
    npx = hdr["width"] * hdr["height"]
    payloads = {}
    for i, L in enumerate(p["layers"]):
        if L["kind"] != 0 or len(L["data"]) != npx * ch:
            continue
        buf = bytearray(L["data"])
        for q in range(npx):
            y, x = divmod(q, hdr["width"])
            cell = (x % 5) * 3 + (y % 3)
            for c in range(cc):
                v = (cell * 2654435761 + (c + 1) * 40503 + (i + 1) * 2246822519) & 0xFFFFFFFF
                buf[q * ch + c] = (v >> 16) & 0xFF
            if ch > cc:
                buf[q * ch + cc] = 255
        payloads[i] = bytes(buf)
    return rebuild(data, payloads) if payloads else None


# --------------------------------------------------------------------------
# seam energy
# --------------------------------------------------------------------------

def luma_map(comp):
    """Integer luma of a flattened document, one byte per pixel."""
    d = doc(comp)
    if d is None or not d["layers"]:
        return None
    w, h, cc, ch = d["w"], d["h"], d["cc"], d["ch"]
    data = d["layers"][0]["data"]
    if len(data) < w * h * ch:
        return None
    if cc == 1:
        return [data[p * ch] for p in range(w * h)], w, h
    lum = []
    for p in range(w * h):
        o = p * ch
        lum.append((LUMA[0] * data[o] + LUMA[1] * data[o + 1] + LUMA[2] * data[o + 2] + 128) >> 8)
    return lum, w, h


def energy_map(lum, w, h):
    """L1 gradient magnitude with clamp-to-edge central differences (SPEC 11.5)."""
    e = [0] * (w * h)
    for y in range(h):
        ym, yp = max(0, y - 1), min(h - 1, y + 1)
        for x in range(w):
            xm, xp = max(0, x - 1), min(w - 1, x + 1)
            gx = lum[y * w + xp] - lum[y * w + xm]
            gy = lum[yp * w + x] - lum[ym * w + x]
            e[y * w + x] = abs(gx) + abs(gy)
    return e


def best_seam_energy(e, w, h):
    """The minimum total energy of a connected top-to-bottom seam."""
    m = list(e[:w])
    for y in range(1, h):
        prev, cur = m, [0] * w
        for x in range(w):
            lo, hi = max(0, x - 1), min(w - 1, x + 1)
            cur[x] = e[y * w + x] + min(prev[lo:hi + 1])
        m = cur
    return min(m)


def _seam_from_candidates(cand, e, w, h):
    """Cheapest connected seam whose row-y column lies in cand[y]; None when none exists."""
    INF = float("inf")
    cur = {}
    for x in cand[0]:
        cur[x] = e[x]
    for y in range(1, h):
        nxt = {}
        for x in cand[y]:
            best = INF
            for c in (x - 1, x, x + 1):
                v = cur.get(c)
                if v is not None and v < best:
                    best = v
            if best < INF:
                nxt[x] = best + e[y * w + x]
        cur = nxt
        if not cur:
            return None
    return min(cur.values()) if cur else None


# --------------------------------------------------------------------------
# gen-fill
# --------------------------------------------------------------------------

def _usable(reg, w, h, r):
    """Positions whose whole (2r+1)^2 patch is on the canvas and clear of the region."""
    acc = [0] * ((w + 1) * (h + 1))
    for y in range(h):
        run = 0
        for x in range(w):
            run += reg[y * w + x]
            acc[(y + 1) * (w + 1) + x + 1] = acc[y * (w + 1) + x + 1] + run
    out = []
    for y in range(r, h - r):
        for x in range(r, w - r):
            x0, y0, x1, y1 = x - r, y - r, x + r + 1, y + r + 1
            s = (acc[y1 * (w + 1) + x1] - acc[y0 * (w + 1) + x1]
                 - acc[y1 * (w + 1) + x0] + acc[y0 * (w + 1) + x0])
            if s == 0:
                out.append(y * w + x)
    return out


def _fill_coherence(px, src_px, reg, w, h, ch, r, usable):
    """(total cost, samples): every filled patch matched against the best legal source patch."""
    offsets = [(dy, dx) for dy in range(-r, r + 1) for dx in range(-r, r + 1)]
    total = 0
    samples = 0
    for p in range(w * h):
        if not reg[p]:
            continue
        y, x = divmod(p, w)
        rel = []
        for dy, dx in offsets:
            ty, tx = y + dy, x + dx
            if 0 <= tx < w and 0 <= ty < h:
                rel.append((ty * w + tx, dy * w + dx))
        samples += len(rel) * ch
        best = None
        for s in usable:
            acc = 0
            for to, off in rel:
                a = to * ch
                b = (s + off) * ch
                for c in range(ch):
                    diff = px[a + c] - src_px[b + c]
                    acc += diff * diff
                if best is not None and acc >= best:
                    break
            if best is None or acc < best:
                best = acc
                if best == 0:
                    break
        total += best or 0
    return total, samples


def check_fill(argv, in_b, ref_b, agent_b):
    fl = flags(argv)
    din, dref, dgot = doc(in_b), doc(ref_b), doc(agent_b)
    if not _same_shape(din, dgot):
        return False, "record list changed"
    if (dgot["w"], dgot["h"]) != (din["w"], din["h"]):
        return False, "gen-fill must not change the canvas size"
    w, h, ch = din["w"], din["h"], din["ch"]
    idx = _int(fl, "index", 0)
    if idx >= len(din["layers"]):
        return False, "verifier: --index out of range"
    reg, why = region(din, fl, idx)
    if reg is None:
        return False, "verifier: " + why
    r = _int(fl, "radius", 2)

    for i, L in enumerate(din["layers"]):
        if i != idx and L["data"] != dgot["layers"][i]["data"]:
            return False, f"record {i} is not the target but changed"
    src_px, px = din["layers"][idx]["data"], dgot["layers"][idx]["data"]
    if len(px) != len(src_px):
        return False, "target payload changed length"

    usable = _usable(reg, w, h, r)
    if not usable:
        return False, "verifier: no usable source position"
    allowed = set()
    for s in usable:
        allowed.add(bytes(src_px[s * ch:(s + 1) * ch]))
    for p in range(w * h):
        if not reg[p]:
            if px[p * ch:(p + 1) * ch] != src_px[p * ch:(p + 1) * ch]:
                return False, f"pixel {p % w},{p // w} is outside the region but changed"
        elif bytes(px[p * ch:(p + 1) * ch]) not in allowed:
            return False, (f"pixel {p % w},{p // w} was invented: "
                           f"{bytes(px[p * ch:(p + 1) * ch]).hex()} is not the value of any "
                           f"legal source position")

    if dref is not None:
        # A fill must carry the variation of the content it copied.  Flattening the region --
        # one legal colour stamped over the whole hole -- satisfies provenance and is still
        # not a fill.  The floor is only applied where the reference's own fill has something
        # to carry, so a flat source stays unconstrained.
        tv_got, tv_pairs = _dirichlet(px, reg, w, h, ch, both=True)
        tv_ref, _ = _dirichlet(dref["layers"][idx]["data"], reg, w, h, ch, both=True)
        if tv_ref > FILL_TV_TOL * tv_pairs * ch and tv_got < FILL_TV_K * tv_ref:
            return False, (f"the filled region carries variation {tv_got}, far below the "
                           f"{FILL_TV_K:g} x {tv_ref} the reference's fill carries: this "
                           f"flattens the region rather than filling it")
    if r > 0 and dref is not None and len(usable) * sum(reg) <= FILL_BUDGET:
        got, samples = _fill_coherence(px, src_px, reg, w, h, ch, r, usable)
        ref, _ = _fill_coherence(dref["layers"][idx]["data"], din["layers"][idx]["data"],
                                 reg, w, h, ch, r, usable)
        bound = FILL_K * ref + FILL_TOL * samples
        if got > bound:
            return False, f"patch coherence {got} exceeds the bound {bound:.0f} (reference {ref})"
    return True, ""


# --------------------------------------------------------------------------
# gen-heal
# --------------------------------------------------------------------------

def _dirichlet(px, reg, w, h, ch, both=False):
    """(energy, pairs): squared difference over adjacencies touching the region.

    `both=True` counts only adjacencies with *both* endpoints inside the region, which
    measures the variation the synthesised content carries rather than how it meets its
    surroundings.
    """
    acc = 0
    pairs = 0
    for y in range(h):
        for x in range(w):
            p = y * w + x
            for q in ((p + 1) if x + 1 < w else -1, (p + w) if y + 1 < h else -1):
                if q < 0:
                    continue
                touches = (reg[p] and reg[q]) if both else (reg[p] or reg[q])
                if not touches:
                    continue
                pairs += 1
                for c in range(ch):
                    diff = px[p * ch + c] - px[q * ch + c]
                    acc += diff * diff
    return acc, pairs


def check_heal(argv, in_b, ref_b, agent_b):
    fl = flags(argv)
    din, dref, dgot = doc(in_b), doc(ref_b), doc(agent_b)
    if not _same_shape(din, dgot):
        return False, "record list changed"
    if (dgot["w"], dgot["h"]) != (din["w"], din["h"]):
        return False, "gen-heal must not change the canvas size"
    w, h, ch = din["w"], din["h"], din["ch"]
    idx = _int(fl, "index", 0)
    if idx >= len(din["layers"]):
        return False, "verifier: --index out of range"
    reg, why = region(din, fl, idx)
    if reg is None:
        return False, "verifier: " + why

    for i, L in enumerate(din["layers"]):
        if i != idx and L["data"] != dgot["layers"][i]["data"]:
            return False, f"record {i} is not the target but changed"
    src_px, px = din["layers"][idx]["data"], dgot["layers"][idx]["data"]
    if len(px) != len(src_px):
        return False, "target payload changed length"

    lo = [255] * ch
    hi = [0] * ch
    for p in range(w * h):
        for c in range(ch):
            v = src_px[p * ch + c]
            if v < lo[c]:
                lo[c] = v
            if v > hi[c]:
                hi[c] = v
    for p in range(w * h):
        if not reg[p]:
            if px[p * ch:(p + 1) * ch] != src_px[p * ch:(p + 1) * ch]:
                return False, f"pixel {p % w},{p // w} is outside the region but changed"
            continue
        for c in range(ch):
            v = px[p * ch + c]
            if v < lo[c] or v > hi[c]:
                return False, (f"pixel {p % w},{p // w} channel {c} = {v} is outside the "
                               f"input's range [{lo[c]}, {hi[c]}]")

    if dref is None:
        return True, ""
    got, pairs = _dirichlet(px, reg, w, h, ch)
    ref, _ = _dirichlet(dref["layers"][idx]["data"], reg, w, h, ch)
    bound = HEAL_K * ref + HEAL_TOL * pairs * ch
    if got > bound:
        return False, f"region discontinuity {got} exceeds the bound {bound:.0f} (reference {ref})"
    return True, ""


# --------------------------------------------------------------------------
# gen-scale
# --------------------------------------------------------------------------

def _removal_cost(in_row, out_row, e_row, k):
    """Cheapest set of k deletions turning in_row into out_row; None when impossible."""
    n, m = len(in_row), len(out_row)
    INF = float("inf")
    prev = [0.0] + [INF] * m          # prev[j] after consuming 0 input columns
    for i in range(1, n + 1):
        cur = [INF] * (m + 1)
        cur[0] = prev[0] + e_row[i - 1]
        for j in range(1, min(i, m) + 1):
            best = prev[j] + e_row[i - 1] if prev[j] != INF else INF
            if in_row[i - 1] == out_row[j - 1] and prev[j - 1] != INF and prev[j - 1] < best:
                best = prev[j - 1]
            cur[j] = best
        prev = cur
    return None if prev[m] == INF or n - m != k else prev[m]


def _insert_positions(in_row, out_row, between):
    """Indices of out_row that can be the spliced pixels, or None when no matching works.

    `between(j)` says whether the pixel at output index j is a legal splice: every channel
    of every record between the neighbours it sits on.  Returns the union over all valid
    matchings, which is what the seam check then searches.
    """
    n, m = len(in_row), len(out_row)
    reach = [[False] * (m + 1) for _ in range(n + 1)]
    reach[0][0] = True
    for i in range(n + 1):
        for j in range(m + 1):
            if not reach[i][j]:
                continue
            if i < n and j < m and in_row[i] == out_row[j]:
                reach[i + 1][j + 1] = True
            if j < m and between(j):
                reach[i][j + 1] = True
    if not reach[n][m]:
        return None
    back = [[False] * (m + 1) for _ in range(n + 1)]
    back[n][m] = True
    for i in range(n, -1, -1):
        for j in range(m, -1, -1):
            if not back[i][j]:
                continue
            if i and j and in_row[i - 1] == out_row[j - 1] and reach[i - 1][j - 1]:
                back[i - 1][j - 1] = True
            if j and reach[i][j - 1] and between(j - 1):
                back[i][j - 1] = True
    out = set()
    for i in range(n + 1):
        for j in range(1, m + 1):
            if back[i][j] and reach[i][j - 1] and between(j - 1):
                out.add(j - 1)
    return out


def check_scale(argv, in_b, ref_b, agent_b, comp=None):
    fl = flags(argv)
    din, dref, dgot = doc(in_b), doc(ref_b), doc(agent_b)
    if not _same_shape(din, dgot):
        return False, "record list changed"
    target = _int(fl, "w", din["w"])
    jitter = _int(fl, "jitter", 0)
    w, h = din["w"], din["h"]
    if dgot["h"] != h:
        return False, "gen-scale must not change the height"
    if dgot["w"] != target:
        return False, f"output width {dgot['w']} != --w {target}"
    if target == w:
        for i, L in enumerate(din["layers"]):
            if L["data"] != dgot["layers"][i]["data"]:
                return False, "--w equal to the current width must leave the pixels alone"
        return True, ""

    rin, rgot = _rows(din, w, h), _rows(dgot, target, h)
    e = None
    if comp is not None:
        lm = luma_map(comp)
        if lm is not None and lm[1] == w and lm[2] == h:
            e = energy_map(lm[0], w, h)

    if target < w:
        delta = w - target
        if e is None:
            return False, "verifier: no energy map"
        if delta == 1:
            cand = []
            for y in range(h):
                lo, hi = _cut_candidates(rin[y], rgot[y])
                if lo > hi:
                    return False, f"row {y} of the output is not the input row minus one pixel"
                cand.append(range(lo, hi + 1))
            got = _seam_from_candidates(cand, e, w, h)
            if got is None:
                return False, "the removed columns do not form a connected seam"
            opt = best_seam_energy(e, w, h)
            bound = opt + 2 * jitter * (h - 1)
            if got > bound:
                return False, (f"the removed seam carries energy {got}; the best seam carries "
                               f"{opt} and the allowance at --jitter {jitter} is {bound}")
            return True, ""
        if dref is None:
            return True, ""
        got = 0
        for y in range(h):
            c = _removal_cost(rin[y], rgot[y], e[y * w:(y + 1) * w], delta)
            if c is None:
                return False, f"row {y} of the output is not the input row with {delta} pixels removed"
            got += c
        ref = 0
        rref = _rows(dref, target, h)
        for y in range(h):
            c = _removal_cost(rin[y], rref[y], e[y * w:(y + 1) * w], delta)
            ref += c if c is not None else 0
        bound = SCALE_K * ref + SCALE_TOL * delta * h
        if got > bound:
            return False, f"removed-column energy {got} exceeds the bound {bound:.0f} (reference {ref})"
        return True, ""

    # insertion
    delta = target - w
    recs = [(dgot["layers"][i]["data"], bpp) for i, bpp in pix_records(dgot)]
    per_row = []
    for y in range(h):
        def between(j, y=y):
            for data, bpp in recs:
                o = (y * target + j) * bpp
                left = (y * target + j - 1) * bpp if j > 0 else None
                right = (y * target + j + 1) * bpp if j + 1 < target else None
                for c in range(bpp):
                    v = data[o + c]
                    a = data[left + c] if left is not None else None
                    b = data[right + c] if right is not None else None
                    if a is None and b is None:
                        return False
                    if a is None:
                        if v != b:
                            return False
                    elif b is None:
                        if v != a:
                            return False
                    elif not (min(a, b) <= v <= max(a, b)):
                        return False
            return True
        pos = _insert_positions(rin[y], rgot[y], between)
        if pos is None:
            return False, (f"row {y} is not the input row with {delta} spliced pixels, each "
                           f"between the neighbours it was averaged from")
        per_row.append(pos)
    if delta == 1 and e is not None:
        # An inserted pixel at output index j was spliced at input column j, so an index at
        # the far end of the row (only reachable when the last pixels repeat) is not one.
        cols = [sorted(c for c in pos if c < w) for pos in per_row]
        if any(not c for c in cols):
            return True, ""
        got = _seam_from_candidates(cols, e, w, h)
        if got is None:
            return False, "the inserted columns do not form a connected seam"
        opt = best_seam_energy(e, w, h)
        bound = opt + 2 * jitter * (h - 1)
        if got > bound:
            return False, (f"the inserted seam carries energy {got}; the best seam carries "
                           f"{opt} and the allowance at --jitter {jitter} is {bound}")
    return True, ""


# --------------------------------------------------------------------------
# gen-extend
# --------------------------------------------------------------------------

def _line(data, bpp, w, horiz, i, t):
    o = ((t * w + i) if horiz else (i * w + t)) * bpp
    return data[o:o + bpp]


def check_extend(argv, in_b, ref_b, agent_b, comp=None):
    fl = flags(argv)
    din, dref, dgot = doc(in_b), doc(ref_b), doc(agent_b)
    if not _same_shape(din, dgot):
        return False, "record list changed"
    direction = fl.get("dir", "")
    amount = _int(fl, "amount", 0)
    w, h = din["w"], din["h"]
    horiz = direction in ("left", "right")
    before = direction in ("left", "top")
    step = -1 if before else 1
    nw = w + amount if horiz else w
    nh = h if horiz else h + amount
    if (dgot["w"], dgot["h"]) != (nw, nh):
        return False, f"output canvas {dgot['w']}x{dgot['h']} != {nw}x{nh}"
    dx = amount if (horiz and before) else 0
    dy = amount if (not horiz and before) else 0

    pin = [(din["layers"][i]["data"], bpp) for i, bpp in pix_records(din)]
    pgot = [(dgot["layers"][i]["data"], bpp) for i, bpp in pix_records(dgot)]
    if len(pin) != len(pgot):
        return False, "pixel-bearing record count changed"

    for (sdata, bpp), (gdata, gbpp) in zip(pin, pgot):
        if bpp != gbpp or len(gdata) != nw * nh * bpp:
            return False, "record payload has the wrong length for the new canvas"
        for y in range(h):
            so = y * w * bpp
            go = ((y + dy) * nw + dx) * bpp
            if gdata[go:go + w * bpp] != sdata[so:so + w * bpp]:
                return False, f"row {y} of the original content was not carried over unchanged"

    lines = w if horiz else h
    linelen = h if horiz else w
    lo = 1 if step > 0 else 0
    hi = lines - 1 if step > 0 else lines - 2
    cands = []
    for j in range(amount):
        if horiz:
            npos = amount - 1 - j if before else w + dx + j
        else:
            npos = amount - 1 - j if before else h + dy + j
        ok = []
        for s in range(lines):
            good = True
            for (sdata, bpp), (gdata, _g) in zip(pin, pgot):
                for t in range(linelen):
                    if horiz:
                        go = ((t + dy) * nw + npos) * bpp
                    else:
                        go = (npos * nw + dx + t) * bpp
                    if gdata[go:go + bpp] != _line(sdata, bpp, w, horiz, s, t):
                        good = False
                        break
                if not good:
                    break
            if good:
                ok.append(s)
        if not ok:
            return False, f"new line {j} is not a copy of any source line"
        if lo <= hi:
            ok = [s for s in ok if lo <= s <= hi]
            if not ok:
                return False, (f"new line {j} copies a source line outside the legal range "
                               f"[{lo}, {hi}]")
        cands.append(ok)

    if comp is None:
        return True, ""
    lm = luma_map(comp)
    dcomp = doc(comp)
    if lm is None or dcomp is None or (dcomp["w"], dcomp["h"]) != (w, h):
        return True, ""
    cdata, cch, cc = dcomp["layers"][0]["data"], dcomp["ch"], dcomp["cc"]

    cache = {}

    def cost(prev, s):
        key = (prev, s)
        if key in cache:
            return cache[key]
        ctx = s - step
        if not (0 <= ctx < lines):
            cache[key] = None
            return None
        acc = 0
        for t in range(linelen):
            a = ((t * w + prev) if horiz else (prev * w + t)) * cch
            b = ((t * w + ctx) if horiz else (ctx * w + t)) * cch
            for c in range(cc):
                diff = cdata[a + c] - cdata[b + c]
                acc += diff * diff
        cache[key] = acc
        return acc

    def chain(sets):
        start = lines - 1 if step > 0 else 0
        cur = {start: 0}
        for j in range(amount):
            nxt = {}
            for s in sets[j]:
                for prev, base in cur.items():
                    c = cost(prev, s)
                    if c is None:
                        continue
                    v = base + c
                    if s not in nxt or v < nxt[s]:
                        nxt[s] = v
            if not nxt:
                return None
            cur = nxt
        return min(cur.values())

    got = chain(cands)
    if got is None:
        return True, ""   # only reachable when no candidate has a predecessor on the canvas
    rcands = []
    dref_pix = [(dref["layers"][i]["data"], bpp) for i, bpp in pix_records(dref)]
    for j in range(amount):
        if horiz:
            npos = amount - 1 - j if before else w + dx + j
        else:
            npos = amount - 1 - j if before else h + dy + j
        ok = []
        for s in range(lines):
            good = True
            for (sdata, bpp), (rdata, _g) in zip(pin, dref_pix):
                for t in range(linelen):
                    if horiz:
                        go = ((t + dy) * nw + npos) * bpp
                    else:
                        go = (npos * nw + dx + t) * bpp
                    if rdata[go:go + bpp] != _line(sdata, bpp, w, horiz, s, t):
                        good = False
                        break
                if not good:
                    break
            if good:
                ok.append(s)
        if not ok:
            return True, ""   # the reference's own strip is unreadable; do not fail the agent
        rcands.append(ok)
    ref = chain(rcands)
    if ref is None:
        return True, ""
    bound = EXT_K * ref + EXT_TOL * amount * linelen * cc
    if got > bound:
        return False, f"continuation cost {got} exceeds the bound {bound:.0f} (reference {ref})"
    return True, ""


# --------------------------------------------------------------------------
# gen-retarget
# --------------------------------------------------------------------------

def _transpose(e, w, h):
    """Row-major w x h -> row-major h x w, so `et[x * h + y] == e[y * w + x]`.

    Every seam routine above reads its map as "rows of `w`, `h` of them" and looks for a
    path with one pixel per row that moves at most one column between rows.  Feed it the
    transposed map and the very same code finds a path with one pixel per COLUMN of the
    original that moves at most one row between columns -- which is exactly a horizontal
    seam.  gen-retarget is therefore judged by the same dynamic program as gen-scale, with
    no second implementation to keep in step.
    """
    return [e[y * w + x] for x in range(w) for y in range(h)]


def check_retarget(argv, in_b, ref_b, agent_b, comp=None):
    fl = flags(argv)
    din, dref, dgot = doc(in_b), doc(ref_b), doc(agent_b)
    if not _same_shape(din, dgot):
        return False, "record list changed"
    target = _int(fl, "h", din["h"])
    jitter = _int(fl, "jitter", 0)
    w, h = din["w"], din["h"]
    if dgot["w"] != w:
        return False, "gen-retarget must not change the width"
    if dgot["h"] != target:
        return False, f"output height {dgot['h']} != --h {target}"
    if target == h:
        for i, L in enumerate(din["layers"]):
            if L["data"] != dgot["layers"][i]["data"]:
                return False, "--h equal to the current height must leave the pixels alone"
        return True, ""

    cin, cgot = _cols(din, w, h), _cols(dgot, w, target)
    et = None
    if comp is not None:
        lm = luma_map(comp)
        if lm is not None and lm[1] == w and lm[2] == h:
            et = _transpose(energy_map(lm[0], w, h), w, h)

    if target < h:
        delta = h - target
        if et is None:
            return False, "verifier: no energy map"
        if delta == 1:
            cand = []
            for x in range(w):
                lo, hi = _cut_candidates(cin[x], cgot[x])
                if lo > hi:
                    return False, f"column {x} of the output is not the input column minus one pixel"
                cand.append(range(lo, hi + 1))
            got = _seam_from_candidates(cand, et, h, w)
            if got is None:
                return False, "the removed rows do not form a connected seam"
            opt = best_seam_energy(et, h, w)
            bound = opt + 2 * jitter * (w - 1)
            if got > bound:
                return False, (f"the removed seam carries energy {got}; the best seam carries "
                               f"{opt} and the allowance at --jitter {jitter} is {bound}")
            return True, ""
        if dref is None:
            return True, ""
        got = 0
        for x in range(w):
            c = _removal_cost(cin[x], cgot[x], et[x * h:(x + 1) * h], delta)
            if c is None:
                return False, f"column {x} of the output is not the input column with {delta} pixels removed"
            got += c
        ref = 0
        cref = _cols(dref, w, target)
        for x in range(w):
            c = _removal_cost(cin[x], cref[x], et[x * h:(x + 1) * h], delta)
            ref += c if c is not None else 0
        bound = RETARGET_K * ref + RETARGET_TOL * delta * w
        if got > bound:
            return False, f"removed-row energy {got} exceeds the bound {bound:.0f} (reference {ref})"
        return True, ""

    # insertion
    delta = target - h
    recs = [(dgot["layers"][i]["data"], bpp) for i, bpp in pix_records(dgot)]
    per_col = []
    for x in range(w):
        def between(j, x=x):
            for data, bpp in recs:
                o = (j * w + x) * bpp
                up = ((j - 1) * w + x) * bpp if j > 0 else None
                dn = ((j + 1) * w + x) * bpp if j + 1 < target else None
                for c in range(bpp):
                    v = data[o + c]
                    a = data[up + c] if up is not None else None
                    b = data[dn + c] if dn is not None else None
                    if a is None and b is None:
                        return False
                    if a is None:
                        if v != b:
                            return False
                    elif b is None:
                        if v != a:
                            return False
                    elif not (min(a, b) <= v <= max(a, b)):
                        return False
            return True
        pos = _insert_positions(cin[x], cgot[x], between)
        if pos is None:
            return False, (f"column {x} is not the input column with {delta} spliced pixels, "
                           f"each between the neighbours it was averaged from")
        per_col.append(pos)
    if delta == 1 and et is not None:
        # An inserted pixel at output index j was spliced at input row j, so an index at the
        # far end of the column (only reachable when the last pixels repeat) is not one.
        rows = [sorted(c for c in pos if c < h) for pos in per_col]
        if any(not c for c in rows):
            return True, ""
        got = _seam_from_candidates(rows, et, h, w)
        if got is None:
            return False, "the inserted rows do not form a connected seam"
        opt = best_seam_energy(et, h, w)
        bound = opt + 2 * jitter * (w - 1)
        if got > bound:
            return False, (f"the inserted seam carries energy {got}; the best seam carries "
                           f"{opt} and the allowance at --jitter {jitter} is {bound}")
    return True, ""


# --------------------------------------------------------------------------
# gen-denoise
# --------------------------------------------------------------------------

def _roughness(px, w, h, ch, cc):
    """(energy, pairs): squared difference across every adjacent pair, colour channels only.

    This is the smoothness measure: a smaller number is a smoother layer.  Alpha is excluded
    because gen-denoise does not touch it.
    """
    acc = 0
    pairs = 0
    for y in range(h):
        for x in range(w):
            p = y * w + x
            for q in ((p + 1) if x + 1 < w else -1, (p + w) if y + 1 < h else -1):
                if q < 0:
                    continue
                pairs += 1
                for c in range(cc):
                    diff = px[p * ch + c] - px[q * ch + c]
                    acc += diff * diff
    return acc, pairs


def _purity(src, px, w, h, ch, cc, reach):
    """Equal neighbourhoods must produce equal outputs.

    A window filter is a pure function of the clamped neighbourhood the window can see plus
    the way that window is clipped by the canvas edge, and of nothing else.  So group the
    pixels by (clip signature, clamped neighbourhood out to `reach`) and require one output
    value per group.  An implementation that lets the clock, an address, an iteration order
    or an unseeded draw into the loop breaks this on any input that repeats itself; a
    deterministic one cannot break it at all.

    `reach` is the search radius plus the patch radius, i.e. the furthest sample the filter
    is allowed to read.  Taking it larger than the implementation's own reach only makes the
    groups finer, which can never produce a false failure.
    """
    groups = {}
    for y in range(h):
        for x in range(w):
            key = [min(x, reach), min(w - 1 - x, reach), min(y, reach), min(h - 1 - y, reach)]
            for dy in range(-reach, reach + 1):
                sy = min(max(y + dy, 0), h - 1)
                for dx in range(-reach, reach + 1):
                    sx = min(max(x + dx, 0), w - 1)
                    o = (sy * w + sx) * ch
                    key.extend(src[o:o + cc])
            k = bytes(key) if all(0 <= v < 256 for v in key) else tuple(key)
            o = (y * w + x) * ch
            val = bytes(px[o:o + cc])
            prev = groups.get(k)
            if prev is None:
                groups[k] = (val, x, y)
            elif prev[0] != val:
                return False, (f"pixel {x},{y} and pixel {prev[1]},{prev[2]} see identical "
                               f"neighbourhoods but were given different values "
                               f"({val.hex()} vs {prev[0].hex()}): the filter is not a pure "
                               f"function of its input")
    return True, ""


def check_denoise(argv, in_b, ref_b, agent_b):
    fl = flags(argv)
    din, dref, dgot = doc(in_b), doc(ref_b), doc(agent_b)
    if not _same_shape(din, dgot):
        return False, "record list changed"
    if (dgot["w"], dgot["h"]) != (din["w"], din["h"]):
        return False, "gen-denoise must not change the canvas size"
    w, h, ch, cc = din["w"], din["h"], din["ch"], din["cc"]
    idx = _int(fl, "index", 0)
    if idx >= len(din["layers"]):
        return False, "verifier: --index out of range"
    radius = _int(fl, "radius", 3)
    strength = _int(fl, "strength", 8)

    # layer discipline: only the target record may move
    for i, L in enumerate(din["layers"]):
        if i != idx and L["data"] != dgot["layers"][i]["data"]:
            return False, f"record {i} is not the target but changed"
    src, px = din["layers"][idx]["data"], dgot["layers"][idx]["data"]
    if len(px) != len(src):
        return False, "target payload changed length"

    # the two documented identities: an empty window, and a blend that admits nothing
    if radius == 0 or strength == 0:
        if px != src:
            return False, (f"--radius {radius} --strength {strength} is the identity, but the "
                           f"target record changed")
        return True, ""

    # alpha rides along untouched: gen-denoise filters the colour channels only
    if ch > cc:
        for p in range(w * h):
            if px[p * ch + cc:(p + 1) * ch] != src[p * ch + cc:(p + 1) * ch]:
                return False, (f"pixel {p % w},{p // w} had its alpha changed; gen-denoise "
                               f"filters the colour channels and leaves alpha alone")

    # provenance: the envelope of values already present in that channel of the layer
    lo = [255] * cc
    hi = [0] * cc
    for p in range(w * h):
        for c in range(cc):
            v = src[p * ch + c]
            if v < lo[c]:
                lo[c] = v
            if v > hi[c]:
                hi[c] = v
    for p in range(w * h):
        for c in range(cc):
            v = px[p * ch + c]
            if v < lo[c] or v > hi[c]:
                return False, (f"pixel {p % w},{p // w} channel {c} = {v} was invented: it is "
                               f"outside the layer's range [{lo[c]}, {hi[c]}]")

    # determinism, in the form one output file can carry
    reach = radius + 1
    if (2 * reach + 1) ** 2 * w * h <= PURITY_BUDGET:
        ok, why = _purity(src, px, w, h, ch, cc, reach)
        if not ok:
            return False, why

    if dref is None:
        return True, ""
    got, pairs = _roughness(px, w, h, ch, cc)
    was, _ = _roughness(src, w, h, ch, cc)
    ref, _ = _roughness(dref["layers"][idx]["data"], w, h, ch, cc)
    # A denoiser may not add detail.  Guarded by the reference so the bound holds for it by
    # construction, exactly like every other bound in this module.
    if ref <= was and got > was:
        return False, (f"the output carries roughness {got}, more than the input's {was}: "
                       f"a denoiser removes detail, it does not add it")
    bound = DENOISE_K * ref + DENOISE_TOL * pairs * cc
    if got > bound:
        return False, (f"the denoised layer carries roughness {got}, above the bound "
                       f"{bound:.0f} the reference's {ref} allows: the layer was not denoised")
    return True, ""


# --------------------------------------------------------------------------
# entry point
# --------------------------------------------------------------------------

CHECKS = {"gen-fill": check_fill, "gen-scale": check_scale,
          "gen-heal": check_heal, "gen-extend": check_extend,
          "gen-retarget": check_retarget, "gen-denoise": check_denoise}
NEEDS_COMPOSITE = ("gen-scale", "gen-extend", "gen-retarget")


def check(op, argv, in_b, ref_b, agent_b, comp=None):
    """(ok, why) for one generative invocation that the reference completed successfully."""
    fn = CHECKS.get(op)
    if fn is None:
        return True, ""
    try:
        if op in NEEDS_COMPOSITE:
            return fn(argv, in_b, ref_b, agent_b, comp)
        return fn(argv, in_b, ref_b, agent_b)
    except Exception as exc:  # noqa: BLE001
        return False, f"property check raised {type(exc).__name__}: {exc}"
