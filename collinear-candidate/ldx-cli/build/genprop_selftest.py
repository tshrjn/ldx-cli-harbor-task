#!/usr/bin/env python3
"""Offline check of tests/genprop.py -- no container, no refldx, no E2B.

    .venv/bin/python build/genprop_selftest.py

`refldx` is a linux/amd64 binary, so on a mac there is no way to run the reference and see
whether the generative property checks accept what it produces.  The four algorithms are
therefore ported here from `build/refldx-src/fam_gen.c`, faithfully, for the one document
shape where the composite equals the single raster layer.  The port is a test fixture and
nothing else: it confirms, in a second rather than a five-minute cloud round trip, that

  - every property in `tests/genprop.py` accepts what the reference really produces, across
    shapes, parameters, and flat, smooth and textured content; and
  - each property rejects the matching wrong implementation -- a fill that invents colours or
    flattens the hole, a carver that takes an arbitrary or disconnected column, an inserter
    that splices a colour of its own, a heal that leaves the region alone or paints it black,
    an extend that replicates the edge line.

It also pins the two legitimate variations the contract must NOT punish: relaxing for 32
sweeps instead of 64, and relaxing for 1000.  `build/wrong_gen_task.py` runs the same
experiment end to end through the real harness; this is the fast loop.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tests"))
import genprop  # noqa: E402

G = genprop

M = 1 << 32


class Rng:
    def __init__(self, seed):
        self.s = (seed * 1664525 + 1013904223) % M

    def draw(self):
        self.s = (self.s * 1664525 + 1013904223) % M
        return self.s >> 2


def fold(d):
    return d ^ (d >> 15)


def pick(d, xb, yb):
    m = fold(d)
    return ((m >> 15) & 0x7fff) & ((1 << xb) - 1), (m & 0x7fff) & ((1 << yb) - 1)


def offset(d, rad):
    span = 2 * rad + 1
    m = fold(d)
    return ((m >> 15) & 0x7fff) % span - rad, (m & 0x7fff) % span - rad


def span1(d, rad):
    m = fold(d)
    return ((m >> 15) & 0x7fff) % (2 * rad + 1) - rad


def bits(v):
    k = 0
    while (1 << k) < v:
        k += 1
    return k


def div_round(n, d):
    return (2 * n + d) // (2 * d)


# ---------------------------------------------------------------- container

def write(w, h, cc, alpha, recs):
    """recs: list of (name, kind, data). kind 0 raster, 3 mask."""
    ch = cc + (1 if alpha else 0)
    out = bytearray(struct.pack("<4sHHIIBBHI", b"LDX1", 1, 1 if alpha else 0, w, h,
                                1 if cc == 3 else 0, 8, len(recs), 72))
    out += b"\0" * 8
    for name, kind, data in recs:
        nb = name.encode()
        out.append(len(nb))
        out += nb
        out += b"\0" * ((4 - (1 + len(nb)) % 4) % 4)
        out += struct.pack("<BBBBhHI", kind, 255, 0, 1, -1, 0, len(data))
        out += bytes(data)
        out += b"\0" * ((4 - len(data) % 4) % 4)
    return bytes(out)


def layer_of(b, i=0):
    return bytearray(genprop.doc(b)["layers"][i]["data"])


def replace(b, i, data):
    d = genprop.doc(b)
    recs = []
    for k, L in enumerate(d["layers"]):
        recs.append((L["name"].decode(), L["kind"], data if k == i else L["data"]))
    return write(d["w"], d["h"], d["cc"], d["ch"] > d["cc"], recs)


def luma(px, w, h, cc, ch):
    if cc == 1:
        return [px[p * ch] for p in range(w * h)]
    return [(77 * px[p * ch] + 150 * px[p * ch + 1] + 29 * px[p * ch + 2] + 128) >> 8
            for p in range(w * h)]


# ---------------------------------------------------------------- reference ports

def ref_fill(b, seed, idx, r, iters, rect):
    d = genprop.doc(b)
    w, h, ch = d["w"], d["h"], d["ch"]
    px = layer_of(b, idx)
    reg = bytearray(w * h)
    x0, y0, rw, rh = rect
    for y in range(y0, y0 + rh):
        for x in range(x0, x0 + rw):
            reg[y * w + x] = 1
    ok = bytearray(w * h)
    ax = ay = -1
    for y in range(h):
        for x in range(w):
            good = x - r >= 0 and y - r >= 0 and x + r < w and y + r < h
            if good:
                for dy in range(-r, r + 1):
                    for dx in range(-r, r + 1):
                        if reg[(y + dy) * w + x + dx]:
                            good = False
                            break
                    if not good:
                        break
            ok[y * w + x] = 1 if good else 0
            if good and ax < 0:
                ax, ay = x, y

    def cost(tx0, ty0, sx, sy):
        acc = 0
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                tx, ty = tx0 + dx, ty0 + dy
                if not (0 <= tx < w and 0 <= ty < h):
                    continue
                to = ty * w + tx
                if reg[to]:
                    continue
                so = (sy + dy) * w + sx + dx
                for c in range(ch):
                    diff = px[to * ch + c] - px[so * ch + c]
                    acc += diff * diff
        return acc

    def src_ok(x, y):
        return 0 <= x < w and 0 <= y < h and ok[y * w + x]

    nn = [(0, 0)] * (w * h)
    rng = Rng(seed)
    wb, hb = bits(w), bits(h)
    for y in range(h):
        for x in range(w):
            p = y * w + x
            if not reg[p]:
                continue
            nn[p] = (ax, ay)
            for _ in range(4):
                cx, cy = pick(rng.draw(), wb, hb)
                if not src_ok(cx, cy):
                    continue
                nn[p] = (cx, cy)
                break
    rad0 = max(w, h)
    for it in range(iters):
        rev = it & 1
        dirx = diry = 1 if rev else -1
        for j in range(h):
            for i in range(w):
                x = w - 1 - i if rev else i
                y = h - 1 - j if rev else j
                p = y * w + x
                if not reg[p]:
                    continue
                bx, by = nn[p]
                best = cost(x, y, bx, by)
                nxq = x + dirx
                if 0 <= nxq < w and reg[y * w + nxq]:
                    cx, cy = nn[y * w + nxq][0] - dirx, nn[y * w + nxq][1]
                    if src_ok(cx, cy):
                        c = cost(x, y, cx, cy)
                        if c < best:
                            best, bx, by = c, cx, cy
                nyq = y + diry
                if 0 <= nyq < h and reg[nyq * w + x]:
                    cx, cy = nn[nyq * w + x][0], nn[nyq * w + x][1] - diry
                    if src_ok(cx, cy):
                        c = cost(x, y, cx, cy)
                        if c < best:
                            best, bx, by = c, cx, cy
                rad = rad0
                while rad >= 1:
                    dx, dy = offset(rng.draw(), rad)
                    cx, cy = bx + dx, by + dy
                    if src_ok(cx, cy):
                        c = cost(x, y, cx, cy)
                        if c < best:
                            best, bx, by = c, cx, cy
                    rad //= 2
                nn[p] = (bx, by)
    for p in range(w * h):
        if not reg[p]:
            continue
        s = nn[p][1] * w + nn[p][0]
        for c in range(ch):
            px[p * ch + c] = px[s * ch + c]
    return replace(b, idx, px)


def ref_seam(px, w, h, cc, ch, rng, jitter):
    lum = luma(px, w, h, cc, ch)
    e = genprop.energy_map(lum, w, h)
    m = list(e[:w])
    back = [[x for x in range(w)]]
    for y in range(1, h):
        cur, bk = [0] * w, [0] * w
        for x in range(w):
            bv, bc = 0, -1
            for c in (x - 1, x, x + 1):
                jit = span1(rng.draw(), jitter)
                if c < 0 or c >= w:
                    continue
                v = m[c] + jit
                if bc < 0 or v < bv:
                    bv, bc = v, c
            cur[x] = e[y * w + x] + bv
            bk[x] = bc
        m, _ = cur, back.append(bk)
    bestc = min(range(w), key=lambda x: (m[x], x))
    seam = [0] * h
    for y in range(h - 1, -1, -1):
        seam[y] = bestc
        bestc = back[y][bestc]
    return seam


def apply_seam(px, w, h, bpp, seam, insert):
    nw = w + 1 if insert else w - 1
    nd = bytearray(nw * h * bpp)
    for y in range(h):
        s = seam[y]
        sr = px[y * w * bpp:(y + 1) * w * bpp]
        dr = nd[y * nw * bpp:(y + 1) * nw * bpp]
        dr[:s * bpp] = sr[:s * bpp]
        if insert:
            lft = s - 1 if s else 0
            for k in range(bpp):
                dr[s * bpp + k] = div_round(sr[lft * bpp + k] + sr[s * bpp + k], 2)
            dr[(s + 1) * bpp:] = sr[s * bpp:]
        else:
            dr[s * bpp:] = sr[(s + 1) * bpp:]
        nd[y * nw * bpp:(y + 1) * nw * bpp] = dr
    return nd, nw


def ref_scale(b, seed, target, jitter=0):
    d = genprop.doc(b)
    w, h, cc, ch = d["w"], d["h"], d["cc"], d["ch"]
    recs = [(L["name"].decode(), L["kind"], bytearray(L["data"])) for L in d["layers"]]
    rng = Rng(seed)
    while w != target:
        insert = w < target
        seam = ref_seam(recs[0][2], w, h, cc, ch, rng, jitter)
        nrecs = []
        for name, kind, data in recs:
            bpp = ch if kind == 0 else (1 if kind == 3 else 0)
            if not bpp:
                nrecs.append((name, kind, data))
                continue
            nd, nw = apply_seam(data, w, h, bpp, seam, insert)
            nrecs.append((name, kind, nd))
        recs = nrecs
        w = nw
    return write(w, h, cc, ch > cc, recs)


def ref_heal(b, seed, idx, radius, rect, sweeps=64):
    d = genprop.doc(b)
    w, h, ch = d["w"], d["h"], d["ch"]
    px = layer_of(b, idx)
    reg = bytearray(w * h)
    x0, y0, rw, rh = rect
    for y in range(y0, y0 + rh):
        for x in range(x0, x0 + rw):
            reg[y * w + x] = 1
    rng = Rng(seed)
    for y in range(h):
        for x in range(w):
            p = y * w + x
            if not reg[p]:
                continue
            for _ in range(4):
                dx, dy = offset(rng.draw(), radius)
                sx, sy = x + dx, y + dy
                if not (0 <= sx < w and 0 <= sy < h):
                    continue
                s = sy * w + sx
                if reg[s]:
                    continue
                for c in range(ch):
                    px[p * ch + c] = px[s * ch + c]
                break
    for _ in range(sweeps):
        for y in range(h):
            for x in range(w):
                p = y * w + x
                if not reg[p]:
                    continue
                for c in range(ch):
                    tot = n = 0
                    if x > 0:
                        tot += px[(p - 1) * ch + c]
                        n += 1
                    if x + 1 < w:
                        tot += px[(p + 1) * ch + c]
                        n += 1
                    if y > 0:
                        tot += px[(p - w) * ch + c]
                        n += 1
                    if y + 1 < h:
                        tot += px[(p + w) * ch + c]
                        n += 1
                    if n:  # a 1x1 canvas divides by zero in C too; not our concern here
                        px[p * ch + c] = div_round(tot, n)
    return replace(b, idx, px)


def ref_extend(b, seed, direction, amount, radius=16, force=None):
    d = genprop.doc(b)
    w, h, cc, ch = d["w"], d["h"], d["cc"], d["ch"]
    horiz = direction in ("left", "right")
    before = direction in ("left", "top")
    step = -1 if before else 1
    nw = w + amount if horiz else w
    nh = h if horiz else h + amount
    dx = amount if (horiz and before) else 0
    dy = amount if (not horiz and before) else 0
    lines = w if horiz else h
    linelen = h if horiz else w
    lo = 1 if step > 0 else 0
    hi = lines - 1 if step > 0 else lines - 2
    comp = layer_of(b, 0)
    rng = Rng(seed)
    prev = lines - 1 if step > 0 else 0
    src = []

    def cost(pv, s):
        ctx = s - step
        acc = 0
        for t in range(linelen):
            a = ((t * w + pv) if horiz else (pv * w + t)) * ch
            bo = ((t * w + ctx) if horiz else (ctx * w + t)) * ch
            for c in range(cc):
                diff = comp[a + c] - comp[bo + c]
                acc += diff * diff
        return acc

    for _j in range(amount):
        if lo > hi:
            src.append(prev)
            continue
        best = prev + step
        if best < lo or best > hi:
            best = lo
        bestc = cost(prev, best)
        for _k in range(12):
            cand = prev + span1(rng.draw(), radius)
            if cand < lo or cand > hi:
                continue
            c = cost(prev, cand)
            if c < bestc:
                bestc, best = c, cand
        src.append(best)
        prev = best
    if force is not None:
        src = [force] * amount
    recs = []
    for L in d["layers"]:
        bpp = ch if L["kind"] == 0 else (1 if L["kind"] == 3 else 0)
        if not bpp:
            recs.append((L["name"].decode(), L["kind"], L["data"]))
            continue
        sd = L["data"]
        nd = bytearray(nw * nh * bpp)
        for y in range(h):
            so = y * w * bpp
            do = ((y + dy) * nw + dx) * bpp
            nd[do:do + w * bpp] = sd[so:so + w * bpp]
        for j in range(amount):
            if horiz:
                nx = amount - 1 - j if before else w + dx + j
                for t in range(h):
                    do = ((t + dy) * nw + nx) * bpp
                    so = (t * w + src[j]) * bpp
                    nd[do:do + bpp] = sd[so:so + bpp]
            else:
                ny = amount - 1 - j if before else h + dy + j
                do = (ny * nw + dx) * bpp
                so = src[j] * w * bpp
                nd[do:do + w * bpp] = sd[so:so + w * bpp]
        recs.append((L["name"].decode(), L["kind"], nd))
    return write(nw, nh, cc, ch > cc, recs)


# ---------------------------------------------------------------- fixtures

def tex_doc(w=14, h=10, f=lambda x, y, c: (29 * x + 53 * y + 91 * c) % 256):
    data = bytearray()
    for y in range(h):
        for x in range(w):
            for c in range(3):
                data.append(f(x, y, c))
            data.append(255)
    return write(w, h, 3, True, [("Tex", 0, data)])


def corridor_doc(w=16, h=8):
    data = bytearray()
    for y in range(h):
        for x in range(w):
            v = 128 if 6 <= x <= 8 else (x * 61 + y * 29) * 7 % 256
            data += bytes([v, v, v, 255])
    return write(w, h, 3, True, [("Corridor", 0, data)])



# --------------------------------------------------------------- margin sweep

def flatdoc(w, h):
    return write(w, h, 3, True, [("L", 0, bytearray(b"\xd7\x46\x44\xff" * (w * h)))])


def smoothdoc(w, h):
    data = bytearray()
    for y in range(h):
        for x in range(w):
            data += bytes([(8 * x + 5 * y) & 255, (200 - 6 * x) & 255, (30 + 4 * y) & 255, 255])
    return write(w, h, 3, True, [("L", 0, data)])


def sweep():
    """Every property must accept the reference on every shape, and reject the wrong
    implementations wherever their output is not identical to a legitimate one."""
    bad = caught = missed = 0
    misses = []
    for (w, h) in ((20, 20), (13, 13), (17, 6), (9, 5), (8, 6), (4, 4), (3, 2)):
        for base, label in ((flatdoc(w, h), "flat"), (smoothdoc(w, h), "smooth")):
            for tex in (False, True):
                doc = G.texturize(base) if tex else base
                if doc is None:
                    continue
                d = G.doc(doc)
                ch = d["ch"]
                src = d["layers"][0]["data"]
                tag = f"{label}{'+tex' if tex else ''} {w}x{h}"
                rw, rh, r = max(1, w // 3), max(1, h // 3), 2
                if w >= 2 * r + 2 and h >= 2 * r + 1:
                    reg, _ = G.region(d, {"x": "0", "y": "0", "w": str(rw), "h": str(rh)}, 0)
                    usable = G._usable(reg, w, h, r)
                    if usable:
                        argv = ["gen-fill", "--seed", 5, "--index", 0, "--radius", r, "--iters", 4,
                                "--x", 0, "--y", 0, "--w", rw, "--h", rh, "IN.ldx", "OUT.ldx"]
                        ref = ref_fill(doc, 5, 0, r, 4, (0, 0, rw, rh))
                        ok, why = G.check("gen-fill", argv, doc, ref, ref, doc)
                        if not ok:
                            bad += 1
                            print(f"[REF-BAD] fill {tag}: {why[:110]}")
                        px = bytearray(src)
                        sp = bytes(px[usable[0] * ch:(usable[0] + 1) * ch])
                        for y in range(rh):
                            for x in range(rw):
                                px[(y * w + x) * ch:(y * w + x + 1) * ch] = sp
                        ok, _ = G.check("gen-fill", argv, doc, ref, replace(doc, 0, px), doc)
                        if tex:
                            caught += (not ok)
                            missed += ok
                            if ok:
                                misses.append(f"fill/constant {tag}")
                if w > 2:
                    for target, jit in ((w - 1, 0), (w - 1, 255), (max(1, w // 2), 0), (w + 1, 0), (w + 2, 3)):
                        argv = ["gen-scale", "--seed", 5, "--w", target, "--jitter", jit, "IN.ldx", "OUT.ldx"]
                        ref = ref_scale(doc, 5, target, jit)
                        ok, why = G.check("gen-scale", argv, doc, ref, ref, doc)
                        if not ok:
                            bad += 1
                            print(f"[REF-BAD] scale {tag} w={target} j={jit}: {why[:110]}")
                    if tex:
                        argv = ["gen-scale", "--seed", 5, "--w", w - 1, "--jitter", 0, "IN.ldx", "OUT.ldx"]
                        ref = ref_scale(doc, 5, w - 1, 0)
                        ok, _ = G.check("gen-scale", argv, doc, ref,
                                        ref_scale_fixed(doc, w - 1, column=lambda y, wd: wd - 1), doc)
                        caught += (not ok)
                        missed += ok
                        if ok:
                            misses.append(f"scale/last-col {tag}")
                for rect in ((0, 0, min(3, w), min(3, h)), (0, 0, max(1, w // 3), max(1, h // 3)), (0, 0, w, h)):
                    argv = ["gen-heal", "--seed", 5, "--index", 0, "--radius", 8,
                            "--x", 0, "--y", 0, "--w", rect[2], "--h", rect[3], "IN.ldx", "OUT.ldx"]
                    ref = ref_heal(doc, 5, 0, 8, rect)
                    for sw in (64, 32, 1000):
                        ok, why = G.check("gen-heal", argv, doc, ref, ref_heal(doc, 5, 0, 8, rect, sw), doc)
                        if not ok:
                            bad += 1
                            print(f"[REF-BAD] heal {tag} {rect} sw={sw}: {why[:110]}")
                    if tex:
                        ok, _ = G.check("gen-heal", argv, doc, ref, doc, doc)
                        caught += (not ok)
                        missed += ok
                        if ok:
                            misses.append(f"heal/untouched {tag} {rect}")
                for dirn, amt in (("right", 1), ("right", 3), ("top", 2), ("bottom", 2), ("left", 2)):
                    argv = ["gen-extend", "--seed", 5, "--dir", dirn, "--amount", amt, "IN.ldx", "OUT.ldx"]
                    ref = ref_extend(doc, 5, dirn, amt)
                    ok, why = G.check("gen-extend", argv, doc, ref, ref, doc)
                    if not ok:
                        bad += 1
                        print(f"[REF-BAD] extend {tag} {dirn} n={amt}: {why[:110]}")
                    if tex:
                        lines = w if dirn in ("left", "right") else h
                        edge = lines - 1 if dirn in ("right", "bottom") else 0
                        ok, _ = G.check("gen-extend", argv, doc, ref,
                                        ref_extend(doc, 5, dirn, amt, force=edge), doc)
                        caught += (not ok)
                        missed += ok
                        if ok:
                            misses.append(f"extend/edge {tag} {dirn} n={amt}")

    print(f"\nreference failures: {bad}")
    print(f"wrong implementations caught: {caught}   missed: {missed}")
    # Every miss is gen-extend on a canvas with too few lines to carry a continuation, or one
    # where the tile period divides the height so the edge line is byte-identical to a line a
    # correct implementation would also have chosen.  Identical output must be graded
    # identically, so those are not failures of the check.
    for m in misses:
        print("  missed:", m)
    return bad == 0


def report(label, ok, why, want):
    verdict = "PASS" if ok else "FAIL"
    good = (ok == want)
    print(f"  [{'ok ' if good else 'BAD'}] {label}: {verdict} {why[:150]}")
    return good


def main():
    allgood = True
    tex = tex_doc()
    stripe = corridor_doc()

    print("gen-fill")
    rect = (4, 3, 5, 4)
    argv = ["gen-fill", "--seed", 777, "--index", 0, "--x", 4, "--y", 3, "--w", 5, "--h", 4,
            "--iters", 3, "--radius", 2, "IN.ldx", "OUT.ldx"]
    ref = ref_fill(tex, 777, 0, 2, 3, rect)
    allgood &= report("reference", *genprop.check("gen-fill", argv, tex, ref, ref), want=True)
    # wrong 1: invent a colour
    px = layer_of(tex)
    d = genprop.doc(tex)
    for y in range(3, 7):
        for x in range(4, 9):
            p = (y * d["w"] + x) * d["ch"]
            px[p:p + 4] = bytes([7, 7, 7, 255])
    allgood &= report("invents colours", *genprop.check("gen-fill", argv, tex, ref, replace(tex, 0, px)), want=False)
    # wrong 2: legal values, but one constant copied everywhere
    px = layer_of(tex)
    anchor = bytes(px[(2 * d["w"] + 2) * 4:(2 * d["w"] + 2) * 4 + 4])
    for y in range(3, 7):
        for x in range(4, 9):
            p = (y * d["w"] + x) * 4
            px[p:p + 4] = anchor
    allgood &= report("constant fill from a legal source", *genprop.check("gen-fill", argv, tex, ref, replace(tex, 0, px)), want=False)
    # wrong 3: touches a pixel outside the region
    px = bytearray(genprop.doc(ref)["layers"][0]["data"])
    px[0] ^= 0x40
    allgood &= report("writes outside the region", *genprop.check("gen-fill", argv, tex, ref, replace(tex, 0, px)), want=False)

    print("gen-scale (carve one seam)")
    argv = ["gen-scale", "--seed", 12345, "--w", 15, "IN.ldx", "OUT.ldx"]
    ref = ref_scale(stripe, 12345, 15)
    allgood &= report("reference", *genprop.check("gen-scale", argv, stripe, ref, ref, stripe), want=True)
    wrong = ref_scale_fixed(stripe, 15, column=lambda y, w: w - 1)
    allgood &= report("removes the last column", *genprop.check("gen-scale", argv, stripe, ref, wrong, stripe), want=False)
    wrong = ref_scale_fixed(stripe, 15, column=lambda y, w: 5)
    allgood &= report("removes column 5", *genprop.check("gen-scale", argv, stripe, ref, wrong, stripe), want=False)
    wrong = ref_scale_fixed(stripe, 15, column=lambda y, w: 0 if y % 2 else 3)
    allgood &= report("disconnected seam", *genprop.check("gen-scale", argv, stripe, ref, wrong, stripe), want=False)

    print("gen-scale (carve three seams on texture)")
    argv = ["gen-scale", "--seed", 5, "--w", 11, "--jitter", 4, "IN.ldx", "OUT.ldx"]
    ref = ref_scale(tex, 5, 11, 4)
    allgood &= report("reference", *genprop.check("gen-scale", argv, tex, ref, ref, tex), want=True)
    wrong = ref_scale_fixed(tex, 11, column=lambda y, w: w - 1)
    # Expected to pass: this texture's energy is near-uniform, so the last column really is
    # almost as cheap as the optimum, and the multi-seam bound is a ceiling rather than a
    # discriminator.  Discrimination comes from the one-seam case, which is absolute.
    allgood &= report("removes the last column (loose multi-seam ceiling)",
                      *genprop.check("gen-scale", argv, tex, ref, wrong, tex), want=True)

    print("gen-scale (insert a seam)")
    argv = ["gen-scale", "--seed", 9, "--w", 15, "IN.ldx", "OUT.ldx"]
    ref = ref_scale(tex, 9, 15)
    allgood &= report("reference", *genprop.check("gen-scale", argv, tex, ref, ref, tex), want=True)
    wrong = insert_wrong(tex, 15)
    allgood &= report("splices an invented pixel", *genprop.check("gen-scale", argv, tex, ref, wrong, tex), want=False)

    print("gen-scale (insert three seams)")
    argv = ["gen-scale", "--seed", 9, "--w", 17, "IN.ldx", "OUT.ldx"]
    ref = ref_scale(tex, 9, 17)
    allgood &= report("reference", *genprop.check("gen-scale", argv, tex, ref, ref, tex), want=True)

    print("gen-heal")
    argv = ["gen-heal", "--seed", 4242, "--index", 0, "--x", 2, "--y", 2, "--w", 9, "--h", 6,
            "IN.ldx", "OUT.ldx"]
    ref = ref_heal(tex, 4242, 0, 8, (2, 2, 9, 6))
    allgood &= report("reference", *genprop.check("gen-heal", argv, tex, ref, ref), want=True)
    allgood &= report("32 sweeps instead of 64",
                      *genprop.check("gen-heal", argv, tex, ref, ref_heal(tex, 4242, 0, 8, (2, 2, 9, 6), 32)), want=True)
    allgood &= report("1000 sweeps instead of 64",
                      *genprop.check("gen-heal", argv, tex, ref, ref_heal(tex, 4242, 0, 8, (2, 2, 9, 6), 1000)), want=True)
    allgood &= report("leaves the region alone", *genprop.check("gen-heal", argv, tex, ref, tex), want=False)
    px = layer_of(tex)
    dd = genprop.doc(tex)
    for y in range(2, 8):
        for x in range(2, 11):
            p = (y * dd["w"] + x) * 4
            px[p:p + 4] = bytes([0, 0, 0, 0])
    allgood &= report("paints the region black", *genprop.check("gen-heal", argv, tex, ref, replace(tex, 0, px)), want=False)

    print("gen-extend")
    argv = ["gen-extend", "--seed", 1, "--dir", "right", "--amount", 5, "IN.ldx", "OUT.ldx"]
    ref = ref_extend(tex, 1, "right", 5)
    allgood &= report("reference", *genprop.check("gen-extend", argv, tex, ref, ref, tex), want=True)
    allgood &= report("replicates the edge column",
                      *genprop.check("gen-extend", argv, tex, ref, ref_extend(tex, 1, "right", 5, force=13), tex), want=False)
    allgood &= report("copies one arbitrary column",
                      *genprop.check("gen-extend", argv, tex, ref, ref_extend(tex, 1, "right", 5, force=4), tex), want=False)
    argv = ["gen-extend", "--seed", 1, "--dir", "top", "--amount", 4, "IN.ldx", "OUT.ldx"]
    ref = ref_extend(tex, 1, "top", 4)
    allgood &= report("reference, --dir top", *genprop.check("gen-extend", argv, tex, ref, ref, tex), want=True)

    print("\n---- margin sweep ----")
    allgood &= sweep()
    print("\n" + ("ALL EXPECTED" if allgood else "SOME OUTCOMES WERE NOT AS EXPECTED"))
    return 0 if allgood else 1


def ref_scale_fixed(b, target, column):
    """A carver that removes a chosen column per row instead of a minimum-energy seam."""
    d = genprop.doc(b)
    w, h, cc, ch = d["w"], d["h"], d["cc"], d["ch"]
    recs = [(L["name"].decode(), L["kind"], bytearray(L["data"])) for L in d["layers"]]
    while w > target:
        seam = [min(max(0, column(y, w)), w - 1) for y in range(h)]
        nrecs = []
        for name, kind, data in recs:
            bpp = ch if kind == 0 else (1 if kind == 3 else 0)
            if not bpp:
                nrecs.append((name, kind, data))
                continue
            nd, nw = apply_seam(data, w, h, bpp, seam, 0)
            nrecs.append((name, kind, nd))
        recs, w = nrecs, nw
    return write(w, h, cc, ch > cc, recs)


def insert_wrong(b, target):
    """An inserter that splices a colour of its own invention."""
    d = genprop.doc(b)
    w, h, cc, ch = d["w"], d["h"], d["cc"], d["ch"]
    data = bytearray(d["layers"][0]["data"])
    while w < target:
        nw = w + 1
        nd = bytearray(nw * h * ch)
        for y in range(h):
            sr = data[y * w * ch:(y + 1) * w * ch]
            dr = bytearray()
            dr += sr[:3 * ch]
            dr += bytes([200, 5, 200, 255])
            dr += sr[3 * ch:]
            nd[y * nw * ch:(y + 1) * nw * ch] = dr
        data, w = nd, nw
    return write(w, h, cc, ch > cc, [("Tex", 0, data)])


if __name__ == "__main__":
    sys.exit(main())
