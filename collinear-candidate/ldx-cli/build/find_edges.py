#!/usr/bin/env python3
"""Find sharp edges empirically.

An edge is only worth planting if a *plausible alternative implementation* — the one a
competent engineer would write from SPEC.md alone — disagrees with the reference on an input
that is easy to construct. This script encodes the alternatives explicitly, then searches for
the smallest input that separates them from `refldx`.

For each probe it reports: whether the reference matches the "textbook" reading, how large the
divergence is, and the minimal witness. Edges where several independent alternatives all
disagree with the reference are the sharpest, because an agent has to *measure* rather than
assume.

Usage:  .venv/bin/python build/find_edges.py [path-to-refldx]
"""
import itertools
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REF = Path(sys.argv[1] if len(sys.argv) > 1 else "/private/tmp/ldxref").resolve()
TMP = Path(tempfile.mkdtemp())


def run(*args, expect_ok=True):
    p = subprocess.run([str(REF), *map(str, args)], capture_output=True, text=True)
    if expect_ok and p.returncode != 0:
        raise RuntimeError(f"refldx {args} -> {p.returncode}: {p.stderr.strip()}")
    return p


def doc_dump(path):
    """Return (header, [layer dicts with 'data' as bytes])."""
    d = json.loads(run("doc-open", "--dump", path).stdout)
    for L in d["layers"]:
        L["bytes"] = bytes.fromhex(L.get("data", ""))
    return d


def px(path, layer=0):
    return doc_dump(path)["layers"][layer]["bytes"]


# ----------------------------------------------------------------- helpers

def mk(name, *args):
    out = TMP / f"{name}.ldx"
    run(*args, out)
    return out


def flatten(src, tag):
    out = TMP / f"{tag}_flat.ldx"
    run("doc-flatten", src, out)
    return px(out)


# --------------------------------------------------- candidate semantics

def blend_fn(mode, cb, cs):
    if mode == "normal": return cs
    if mode == "multiply": return (cb * cs + 127) // 255
    if mode == "screen": return cb + cs - (cb * cs + 127) // 255
    if mode == "darken": return min(cb, cs)
    if mode == "lighten": return max(cb, cs)
    if mode == "difference": return abs(cb - cs)
    raise ValueError(mode)


def composite(cb, ad, cs, a_s, eff, mode, *, variant):
    """Return (colour, alpha) for one channel under a given reading of the spec."""
    if variant == "reference":
        ae = (a_s * eff + 127) // 255
        if ae == 0: return cb, ad
        oa = ae + ad - (ae * ad + 127) // 255
        b = ((255 - ad) * cs + ad * blend_fn(mode, cb, cs) + 127) // 255
        num = b * ae * 255 + cb * ad * (255 - ae)
        den = oa * 255
        return (2 * num + den) // (2 * den), oa
    if variant == "blend_direct":          # applies the SPEC table verbatim, ignoring backdrop alpha
        ae = (a_s * eff + 127) // 255
        if ae == 0: return cb, ad
        oa = ae + ad - (ae * ad + 127) // 255
        b = blend_fn(mode, cb, cs)
        num = b * ae * 255 + cb * ad * (255 - ae)
        den = oa * 255
        return (2 * num + den) // (2 * den), oa
    if variant == "truncate":              # same maths, floor instead of round-half-up
        ae = (a_s * eff + 127) // 255
        if ae == 0: return cb, ad
        oa = ae + ad - (ae * ad + 127) // 255
        b = ((255 - ad) * cs + ad * blend_fn(mode, cb, cs) + 127) // 255
        return (b * ae * 255 + cb * ad * (255 - ae)) // (oa * 255), oa
    if variant == "premultiplied":         # composite in premultiplied space, unpremultiply at the end
        ae = (a_s * eff + 127) // 255
        if ae == 0: return cb, ad
        oa = ae + ad - (ae * ad + 127) // 255
        b = ((255 - ad) * cs + ad * blend_fn(mode, cb, cs) + 127) // 255
        pm = (b * ae + 127) // 255 + ((cb * ad + 127) // 255 * (255 - ae) + 127) // 255
        return min(255, (pm * 255 + oa // 2) // oa if oa else 0), oa
    if variant == "opacity_trunc":         # scales alpha by opacity without rounding
        ae = (a_s * eff) // 255
        if ae == 0: return cb, ad
        oa = ae + ad - (ae * ad + 127) // 255
        b = ((255 - ad) * cs + ad * blend_fn(mode, cb, cs) + 127) // 255
        num = b * ae * 255 + cb * ad * (255 - ae)
        den = oa * 255
        return (2 * num + den) // (2 * den), oa
    raise ValueError(variant)


VARIANTS = ["reference", "blend_direct", "truncate", "premultiplied", "opacity_trunc"]


# ----------------------------------------------------------------- probes

def probe_blend_over_partial_alpha():
    """Two opaque-ish layers where the BACKDROP is partially transparent.

    SPEC.md gives the blend table 'for a fully opaque destination'. The textbook reading is to
    apply that table directly. The reference instead interpolates between the source colour
    and the blended colour by backdrop alpha, so the two readings separate whenever
    0 < ad < 255 and blend != normal.
    """
    rows = []
    for mode in ("multiply", "screen", "darken", "difference"):
        for cb, ad, cs, a_s, eff in itertools.product((0, 40, 128, 200), (1, 64, 128, 200), (0, 60, 190, 255), (255,), (255,)):
            base = mk(f"b_{mode}_{cb}_{ad}", "doc-new", "--w", 1, "--h", 1, "--mode", "rgb", "--fill", f"{cb},{cb},{cb},{ad}")
            top = TMP / "t.ldx"
            run("layer-add", "--name", "T", "--fill", f"{cs},{cs},{cs},{a_s}", "--blend", mode, base, top)
            got = flatten(top, "bp")[0]
            preds = {v: composite(cb, ad, cs, a_s, eff, mode, variant=v)[0] for v in VARIANTS}
            if preds["reference"] != preds["blend_direct"]:
                rows.append((mode, cb, ad, cs, got, preds))
    agree_ref = sum(1 for r in rows if r[4] == r[5]["reference"])
    agree_alt = sum(1 for r in rows if r[4] == r[5]["blend_direct"])
    return {
        "name": "blend over a partially transparent backdrop",
        "separating_inputs": len(rows),
        "matches_reference_model": agree_ref,
        "matches_textbook_model": agree_alt,
        "witness": rows[0] if rows else None,
        "max_divergence": max((abs(r[4] - r[5]["blend_direct"]) for r in rows), default=0),
    }


def probe_group_opacity_accumulation():
    """Nested groups: does opacity round at every level, or once at the end?

    round(round(a*b/255)*c/255)  vs  round(a*b*c/255^2). These differ for many triples; the
    deeper the nesting the further they drift.
    """
    seps = []
    for a, b, c in itertools.product(range(1, 256, 7), repeat=3):
        per_level = ((a * b + 127) // 255)
        per_level = (per_level * c + 127) // 255
        single = (a * b * c + (255 * 255) // 2) // (255 * 255)
        if per_level != single:
            seps.append((a, b, c, per_level, single))
    # measure the reference on the sharpest few
    checked = []
    for a, b, c, pl, sg in seps[:6]:
        base = mk(f"g_{a}_{b}_{c}", "doc-new", "--w", 1, "--h", 1, "--mode", "gray", "--fill", "0,255")
        d1 = TMP / "g1.ldx"; run("layer-add", "--name", "G1", "--kind", "group", "--opacity", a, base, d1)
        d2 = TMP / "g2.ldx"; run("layer-add", "--name", "G2", "--kind", "group", "--parent", 1, "--opacity", b, d1, d2)
        d3 = TMP / "g3.ldx"; run("layer-add", "--name", "L", "--parent", 2, "--fill", "255,255", "--opacity", c, d2, d3)
        out = flatten(d3, "go")
        checked.append({"opacities": (a, b, c), "per_level_pred": pl, "single_round_pred": sg, "observed_alpha": out[1], "observed_value": out[0]})
    return {"name": "nested group opacity: round per level or once",
            "separating_triples": len(seps), "checked": checked}


def probe_mask_vs_opacity_order():
    """mask*opacity rounding is not commutative: round(round(a*m/255)*o/255) vs the other order."""
    seps = [(a, m, o) for a in range(1, 256, 11) for m in range(1, 256, 11) for o in range(1, 256, 11)
            if (((a * m + 127) // 255) * o + 127) // 255 != (((a * o + 127) // 255) * m + 127) // 255]
    witness = seps[0] if seps else None
    obs = None
    if witness:
        a, m, o = witness
        base = mk("mo", "doc-new", "--w", 1, "--h", 1, "--mode", "gray", "--alpha", 1, "--fill", "0,0")
        d1 = TMP / "mo1.ldx"
        run("layer-add", "--name", "L", "--fill", f"255,{a}", "--opacity", o, base, d1)
        d2 = TMP / "mo2.ldx"
        p = subprocess.run([str(REF), "layer-mask-add", "--index", "1", "--fill", str(m), str(d1), str(d2)],
                           capture_output=True, text=True)
        if p.returncode == 0:
            obs = {"observed": list(flatten(d2, "mo")),
                   "mask_then_opacity": (((a * m + 127) // 255) * o + 127) // 255,
                   "opacity_then_mask": (((a * o + 127) // 255) * m + 127) // 255}
        else:
            obs = {"error": p.stderr.strip()[:160]}
    return {"name": "mask before or after layer opacity", "separating_triples": len(seps),
            "witness_alpha_mask_opacity": witness, "observation": obs}


def probe_merge_then_flatten():
    """Is merge-down followed by flatten byte-identical to flattening directly?

    A tempting invariant. If the reference does NOT satisfy it, an agent that assumes it will
    diverge; if it does, an agent that rounds twice will diverge. Either way it is sharp, so
    measure rather than assume.
    """
    results = []
    for mode in ("normal", "multiply", "screen", "difference"):
        for op in (255, 200, 128, 77):
            base = mk(f"m_{mode}_{op}", "doc-new", "--w", 3, "--h", 2, "--mode", "rgb", "--fill", "90,140,210,200")
            d1 = TMP / "m1.ldx"
            run("layer-add", "--name", "A", "--fill", "30,200,90,180", "--blend", mode, "--opacity", op, base, d1)
            d2 = TMP / "m2.ldx"
            run("layer-add", "--name", "B", "--fill", "220,10,60,140", "--blend", "normal", "--opacity", 210, d1, d2)
            direct = flatten(d2, "md")
            merged = TMP / "mm.ldx"
            p = subprocess.run([str(REF), "layer-merge-down", "--index", "2", str(d2), str(merged)],
                               capture_output=True, text=True)
            if p.returncode != 0:
                results.append({"mode": mode, "opacity": op, "error": p.stderr.strip()[:120]})
                continue
            after = flatten(merged, "mf")
            diff = max((abs(x - y) for x, y in zip(direct, after)), default=0)
            results.append({"mode": mode, "opacity": op, "identical": direct == after, "max_channel_diff": diff})
    ident = sum(1 for r in results if r.get("identical"))
    return {"name": "merge-down then flatten vs flatten directly",
            "cases": len(results), "identical_cases": ident, "detail": results[:8]}


def probe_structural_mask_follows_layer():
    """Does layer-reorder carry a layer's mask with it?"""
    base = mk("sm", "doc-new", "--w", 2, "--h", 2, "--mode", "gray", "--fill", "10,255")
    d1 = TMP / "sm1.ldx"; run("layer-add", "--name", "A", "--fill", "100,255", base, d1)
    d2 = TMP / "sm2.ldx"; run("layer-add", "--name", "B", "--fill", "200,255", d1, d2)
    d3 = TMP / "sm3.ldx"
    p = subprocess.run([str(REF), "layer-mask-add", "--index", "1", "--fill", "128", str(d2), str(d3)],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return {"name": "mask follows its layer on reorder", "error": p.stderr.strip()[:160]}
    before = [(L["name"], L["kind"], L["parent"]) for L in doc_dump(d3)["layers"]]
    d4 = TMP / "sm4.ldx"
    p = subprocess.run([str(REF), "layer-reorder", "--from", "1", "--to", "3", str(d3), str(d4)],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return {"name": "mask follows its layer on reorder", "before": before, "error": p.stderr.strip()[:160]}
    after = [(L["name"], L["kind"], L["parent"]) for L in doc_dump(d4)["layers"]]
    idx_a = [i for i, L in enumerate(after) if L[0] == "A"]
    masks = [i for i, L in enumerate(after) if L[1] == "mask"]
    return {"name": "mask follows its layer on reorder", "before": before, "after": after,
            "mask_directly_above_A": bool(idx_a and masks and masks[0] == idx_a[0] + 1)}


def probe_rot_identity():
    """rot270 vs rot90 x3, and rot90 x4 vs identity, on a non-square multi-record document."""
    base = mk("r0", "doc-new", "--w", 5, "--h", 3, "--mode", "rgb", "--fill", "10,20,30,200")
    d1 = TMP / "r1.ldx"; run("layer-add", "--name", "A", "--fill", "200,10,90,128", "--blend", "screen", base, d1)
    a = TMP / "ra.ldx"; run("px-rot270", d1, a)
    t1 = TMP / "rt1.ldx"; t2 = TMP / "rt2.ldx"; t3 = TMP / "rt3.ldx"
    run("px-rot90", d1, t1); run("px-rot90", t1, t2); run("px-rot90", t2, t3)
    four = TMP / "r4.ldx"; run("px-rot90", t3, four)
    return {"name": "rot270 == rot90 x3", "identical": a.read_bytes() == t3.read_bytes(),
            "rot90x4_is_identity": four.read_bytes() == d1.read_bytes()}


def probe_convolve_edge_and_sign():
    """1-pixel-wide canvas with clamp-to-edge: a normalised kernel must be the identity.
    Zero-padding — the other obvious choice — darkens the pixel instead."""
    base = mk("cv", "doc-new", "--w", 1, "--h", 1, "--mode", "gray", "--alpha", 0, "--fill", "200")
    out = TMP / "cv1.ldx"
    p = subprocess.run([str(REF), "px-convolve", "--kernel", "1,1,1,1,1,1,1,1,1", "--shift", "0",
                        str(base), str(out)], capture_output=True, text=True)
    res = {"name": "convolution border handling on a 1x1 canvas"}
    if p.returncode == 0:
        res["observed"] = px(out)[0]
        res["clamp_to_edge_predicts"] = 255  # 9*200 = 1800 -> clamped to 255
        res["zero_pad_predicts"] = 200
    else:
        res["error"] = p.stderr.strip()[:160]
    # negative accumulator rounding
    b2 = mk("cv2", "doc-new", "--w", 1, "--h", 1, "--mode", "gray", "--alpha", 0, "--fill", "3")
    o2 = TMP / "cv2o.ldx"
    p2 = subprocess.run([str(REF), "px-convolve", "--kernel", "0,0,0,0,-128,0,0,0,0", "--shift", "8",
                         "--bias", "128", str(b2), str(o2)], capture_output=True, text=True)
    if p2.returncode == 0:
        res["negative_tie_observed"] = px(o2)[0]
        res["half_away_from_zero_predicts"] = 128 - 2   # -(3*128 + 128)>>8 = -2
        res["arithmetic_shift_predicts"] = 128 - 1      # (-384 + 128) >> 8 = -1
    return res


PROBES = [probe_blend_over_partial_alpha, probe_group_opacity_accumulation, probe_mask_vs_opacity_order,
          probe_merge_then_flatten, probe_structural_mask_follows_layer, probe_rot_identity,
          probe_convolve_edge_and_sign]


def main():
    out = []
    for fn in PROBES:
        try:
            out.append(fn())
        except Exception as e:  # noqa: BLE001
            out.append({"name": fn.__name__, "error": f"{type(e).__name__}: {e}"})
    print(json.dumps(out, indent=1, default=str))


if __name__ == "__main__":
    main()
