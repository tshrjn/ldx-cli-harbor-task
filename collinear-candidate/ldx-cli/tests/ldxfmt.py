"""Lenient LDX container parser used by the verifier (white-box structural checks).

parse(data) never raises: it returns a dict with whatever it could decode plus an
`errors` list.  Two documents are *structurally equal* when every header field and every
record's metadata match, all padding/reserved bytes are zero, and the bytes are identical.
"""
import struct

KINDS = {0: "raster", 1: "group_open", 2: "group_close", 3: "mask"}
BLENDS = {0: "normal", 1: "multiply", 2: "screen", 3: "darken", 4: "lighten", 5: "difference"}


def parse(data: bytes) -> dict:
    d = {"header": None, "layers": [], "errors": [], "padding_zero": True, "reserved_zero": True, "size": len(data)}
    if len(data) < 32:
        d["errors"].append("short header")
        return d
    magic, version, flags, w, h, mode, depth, nlayers, res = struct.unpack_from("<4sHHIIBBHI", data, 0)
    reserved = data[24:32]
    hdr = {"magic": magic, "version": version, "flags": flags, "width": w, "height": h, "mode": mode,
           "bit_depth": depth, "layer_count": nlayers, "resolution": res, "reserved": reserved.hex()}
    d["header"] = hdr
    if magic != b"LDX1":
        d["errors"].append("bad magic")
    if reserved != b"\0" * 8:
        d["reserved_zero"] = False
        d["errors"].append("header reserved bytes not zero")
    has_alpha = flags & 1
    cc = 3 if mode == 1 else 1
    ch = cc + (1 if has_alpha else 0)
    off = 32
    for i in range(nlayers):
        if off + 1 > len(data):
            d["errors"].append(f"record {i}: truncated")
            break
        name_len = data[off]
        need = 1 + name_len
        pad = (4 - need % 4) % 4
        if off + need + pad + 12 > len(data):
            d["errors"].append(f"record {i}: truncated fixed fields")
            break
        name = data[off + 1:off + 1 + name_len]
        pad_bytes = data[off + need:off + need + pad]
        if pad_bytes != b"\0" * pad:
            d["padding_zero"] = False
            d["errors"].append(f"record {i}: name padding not zero")
        off += need + pad
        kind, opacity, blend, lflags, parent, rsv, data_len = struct.unpack_from("<BBBBhHI", data, off)
        if rsv != 0:
            d["reserved_zero"] = False
            d["errors"].append(f"record {i}: record reserved not zero")
        off += 12
        expected = w * h * ch if kind == 0 else (w * h if kind == 3 else 0)
        if data_len != expected:
            d["errors"].append(f"record {i}: data_len {data_len} != expected {expected}")
        if off + data_len > len(data):
            d["errors"].append(f"record {i}: truncated data")
            payload = data[off:]
            off = len(data)
        else:
            payload = data[off:off + data_len]
            off += data_len
        pad = (4 - off % 4) % 4
        pad_bytes = data[off:off + pad]
        if len(pad_bytes) < pad:
            d["errors"].append(f"record {i}: truncated data padding")
        elif pad_bytes != b"\0" * pad:
            d["padding_zero"] = False
            d["errors"].append(f"record {i}: data padding not zero")
        off += pad
        d["layers"].append({"name": name, "kind": kind, "opacity": opacity, "blend": blend, "flags": lflags,
                            "parent": parent, "data_len": data_len, "data": payload})
    if off != len(data):
        d["errors"].append(f"trailing bytes: {len(data) - off}")
    return d


def meta(layer: dict) -> tuple:
    return (layer["name"], layer["kind"], layer["opacity"], layer["blend"], layer["flags"], layer["parent"], layer["data_len"])


def pixels_equal(a: bytes, b: bytes) -> bool:
    """Functional comparison: same layer count and identical payloads in order."""
    pa, pb = parse(a), parse(b)
    if pa["header"] is None or pb["header"] is None:
        return False
    if len(pa["layers"]) != len(pb["layers"]):
        return False
    ha, hb = pa["header"], pb["header"]
    if (ha["width"], ha["height"], ha["mode"], ha["flags"] & 1) != (hb["width"], hb["height"], hb["mode"], hb["flags"] & 1):
        return False
    return all(la["kind"] == lb["kind"] and la["data"] == lb["data"] for la, lb in zip(pa["layers"], pb["layers"]))


def structural_diff(ref: bytes, got: bytes, payload: bool = True) -> list:
    """Return a list of structural differences (empty == structurally identical).

    `payload=False` compares everything except the pixel bytes: the header, the record
    metadata, the zero-fill and the parse itself.  The generative tier is graded that way,
    because its pixels are checked against the properties in `genprop.py` instead.
    """
    diffs = []
    pr, pg = parse(ref), parse(got)
    if pg["header"] is None:
        return ["unparseable header"]
    for k in ("magic", "version", "flags", "width", "height", "mode", "bit_depth", "layer_count", "resolution"):
        if pr["header"][k] != pg["header"][k]:
            diffs.append(f"header.{k}: {pr['header'][k]!r} != {pg['header'][k]!r}")
    if not pg["reserved_zero"]:
        diffs.append("reserved bytes not zero")
    if not pg["padding_zero"]:
        diffs.append("padding not zero")
    if len(pr["layers"]) != len(pg["layers"]):
        diffs.append(f"record count {len(pr['layers'])} != {len(pg['layers'])}")
    for i, (lr, lg) in enumerate(zip(pr["layers"], pg["layers"])):
        if meta(lr) != meta(lg):
            diffs.append(f"record {i} metadata {meta(lr)} != {meta(lg)}")
    for e in pg["errors"]:
        if e not in pr["errors"]:
            diffs.append("parse: " + e)
    if payload and ref != got and not diffs:
        diffs.append("bytes differ (pixel payload)")
    return diffs
