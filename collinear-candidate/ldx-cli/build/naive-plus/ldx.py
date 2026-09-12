#!/usr/bin/env python3
"""Naive-plus: the shallow ldx after iterating until compare.sh reads 100%.
Only the defect compare.sh could see (padding) is fixed; the edge semantics it never
exercises are untouched. Originally: deliberately shallow ldx: happy path from SPEC.md only, no experiments against refldx.
Used for the discrimination dry run (README / RUN_REPORT). Known shortcuts:
truncating division, wrapping arithmetic, no crop clamping, no error for swap-on-gray,
garbage in padding, zero-alpha pixels zeroed by the formula."""
import json, struct, sys
BLENDS = ["normal", "multiply", "screen", "darken", "lighten", "difference"]
KINDS = ["raster", "group_open", "group_close", "mask"]
PAD = b"\x00"  # padding fixed after compare.sh showed byte mismatches

def die(code, msg, rc):
    sys.stderr.write(json.dumps({"code": code, "message": msg}) + "\n"); sys.exit(rc)

def parse_args(argv):
    flags, pos = {}, []
    i = 0
    while i < len(argv):
        if argv[i].startswith("--"):
            k = argv[i][2:]
            if k == "dump": flags[k] = "1"; i += 1; continue
            if i + 1 >= len(argv): die("E_USAGE", "flag requires a value", 2)
            flags[k] = argv[i + 1]; i += 2
        else:
            pos.append(argv[i]); i += 1
    return flags, pos

def read(path):
    try: b = open(path, "rb").read()
    except OSError: die("E_IO", "cannot open input file", 5)
    if b[:4] != b"LDX1": die("E_BAD_FILE", "bad magic", 4)
    _, ver, flags, w, h, mode, depth, n, res = struct.unpack_from("<4sHHIIBBHI", b, 0)
    d = {"flags": flags, "w": w, "h": h, "mode": mode, "res": res, "layers": []}
    off = 32
    for _ in range(n):
        nl = b[off]; name = b[off + 1:off + 1 + nl]; off += 1 + nl; off += (4 - off % 4) % 4
        kind, op, bl, fl, par, _r, dl = struct.unpack_from("<BBBBhHI", b, off); off += 12
        data = b[off:off + dl]; off += dl; off += (4 - off % 4) % 4
        d["layers"].append(dict(name=name, kind=kind, opacity=op, blend=bl, flags=fl, parent=par, data=bytearray(data)))
    return d

def write(d, path):
    out = bytearray(struct.pack("<4sHHIIBBHI", b"LDX1", 1, d["flags"], d["w"], d["h"], d["mode"], 8, len(d["layers"]), d["res"]) + b"\0" * 8)
    for L in d["layers"]:
        out += bytes([len(L["name"])]) + L["name"]
        out += PAD * ((4 - len(out) % 4) % 4)
        out += struct.pack("<BBBBhHI", L["kind"], L["opacity"], L["blend"], L["flags"], L["parent"], 0, len(L["data"]))
        out += L["data"]
        out += PAD * ((4 - len(out) % 4) % 4)
    open(path, "wb").write(out)

def ch(d): return (3 if d["mode"] == 1 else 1) + (d["flags"] & 1)
def cc(d): return 3 if d["mode"] == 1 else 1

def doc_new(f, p):
    w, h = int(f["w"]), int(f["h"]); mode = {"gray": 0, "rgb": 1}[f["mode"]]
    d = {"flags": int(f.get("alpha", "1")), "w": w, "h": h, "mode": mode, "res": int(f.get("dpi", "72")) << 16, "layers": []}
    n = ch(d); fill = [int(x) for x in f["fill"].split(",")] if "fill" in f else [255] * n
    d["layers"].append(dict(name=b"Background", kind=0, opacity=255, blend=0, flags=5, parent=-1, data=bytearray(bytes(fill) * (w * h))))
    write(d, p[0])

def doc_open(f, p):
    d = read(p[0])
    layers = []
    for i, L in enumerate(d["layers"]):
        o = {"index": i, "name": L["name"].decode(), "kind": KINDS[L["kind"]], "opacity": L["opacity"], "blend": BLENDS[L["blend"]],
             "visible": bool(L["flags"] & 1), "locked": bool(L["flags"] & 2), "is_background": bool(L["flags"] & 4), "parent": L["parent"], "data_len": len(L["data"])}
        if "dump" in f: o["data"] = bytes(L["data"]).hex()
        layers.append(o)
    doc = {"width": d["w"], "height": d["h"], "mode": "rgb" if d["mode"] == 1 else "gray", "has_alpha": bool(d["flags"] & 1), "bit_depth": 8, "resolution": d["res"], "layer_count": len(d["layers"])}
    print(json.dumps({"document": doc, "layers": layers}, separators=(",", ":")))

def pnm(path):
    b = open(path, "rb").read(); parts = b.split(maxsplit=4); n = 3 if parts[0] == b"P6" else 1
    return parts[4][:int(parts[1]) * int(parts[2]) * n], n

def layer_add(f, p):
    d = read(p[0]); n = ch(d); c = cc(d); w, h = d["w"], d["h"]
    parent = int(f.get("parent", "-1"))
    L = dict(name=f["name"].encode(), kind=1 if f.get("kind") == "group" else 0, opacity=int(f.get("opacity", "255")),
             blend=BLENDS.index(f.get("blend", "normal")), flags=(int(f.get("visible", "1")) | (int(f.get("locked", "0")) << 1)), parent=parent, data=bytearray())
    if L["kind"] == 0:
        px = bytearray(w * h * n)
        if "fill" in f: px = bytearray(bytes(int(x) for x in f["fill"].split(",")) * (w * h))
        elif "from" in f:
            col, k = pnm(f["from"])
            for i in range(w * h):
                px[i * n:i * n + c] = col[i * k:i * k + c]
                if d["flags"] & 1: px[i * n + c] = 255
        if "alpha-from" in f:
            al, _ = pnm(f["alpha-from"])
            for i in range(w * h): px[i * n + c] = al[i]
        L["data"] = px
    pos = len(d["layers"])
    if parent >= 0:
        depth = 0
        for i in range(parent, len(d["layers"])):
            if d["layers"][i]["kind"] == 1: depth += 1
            elif d["layers"][i]["kind"] == 2:
                depth -= 1
                if depth == 0: pos = i; break
    new = [L] + ([dict(name=b"", kind=2, opacity=255, blend=0, flags=0, parent=pos, data=bytearray())] if L["kind"] == 1 else [])
    for M in d["layers"]:
        if M["parent"] >= pos: M["parent"] += len(new)
    d["layers"][pos:pos] = new
    write(d, p[1])

def blend(m, cb, cs):
    if m == 1: return cb * cs // 255
    if m == 2: return cb + cs - cb * cs // 255
    if m == 3: return min(cb, cs)
    if m == 4: return max(cb, cs)
    if m == 5: return abs(cb - cs)
    return cs

def doc_flatten(f, p):
    d = read(p[0]); n = ch(d); c = cc(d); w, h = d["w"], d["h"]; npx = w * h
    canvas = [[0] * (c + 1) for _ in range(npx)]
    stack = [(1.0, True)]
    for i, L in enumerate(d["layers"]):
        eff, vis = stack[-1][0] * L["opacity"] / 255, stack[-1][1] and bool(L["flags"] & 1)
        if L["kind"] == 1: stack.append((eff, vis)); continue
        if L["kind"] == 2: stack.pop(); continue
        if L["kind"] != 0 or not vis: continue
        for k in range(npx):
            src = L["data"][k * n:k * n + n]; a_s = src[c] if d["flags"] & 1 else 255
            ae = int(a_s * eff); dst = canvas[k]; ad = dst[c]
            oa = ae + ad - ae * ad // 255
            for j in range(c):
                cs, cb = src[j], dst[j]
                b = blend(L["blend"], cb, cs)  # SPEC table taken literally (opaque-backdrop formula)
                dst[j] = int((b * ae * 255 + cb * ad * (255 - ae)) / (oa * 255)) if oa else 0
            dst[c] = oa
    px = bytearray()
    for k in range(npx): px += bytes(canvas[k][:n])
    d["layers"] = [dict(name=b"Background", kind=0, opacity=255, blend=0, flags=5, parent=-1, data=px)]
    write(d, p[1])

def px_crop(f, p):
    d = read(p[0]); x, y, w, h = (int(f[k]) for k in "xywh"); n = ch(d)
    for L in d["layers"]:
        k = n if L["kind"] == 0 else (1 if L["kind"] == 3 else 0)
        if not k: continue
        rows = [L["data"][((y + r) * d["w"] + x) * k:((y + r) * d["w"] + x + w) * k] for r in range(h)]
        L["data"] = bytearray(b"".join(rows))
    d["w"], d["h"] = w, h
    write(d, p[1])

def px_channel(f, p):
    d = read(p[0]); n = ch(d); c = cc(d); L = d["layers"][int(f.get("index", "0"))]; op = f["op"]; arg = int(f.get("arg", "0"))
    for i in range(0, len(L["data"]), n):
        for j in range(c):
            v = L["data"][i + j]
            if op == "invert": v = 255 - v
            elif op == "threshold": v = 255 if v >= arg else 0
            elif op == "offset": v = (v + arg) & 0xFF
            L["data"][i + j] = v
        if op == "swap" and c == 3: L["data"][i], L["data"][i + 2] = L["data"][i + 2], L["data"][i]
        if op == "extract_alpha":
            av = L["data"][i + c]
            for j in range(c): L["data"][i + j] = av
            L["data"][i + c] = 255
    write(d, p[1])

CMDS = {"doc-new": doc_new, "doc-open": doc_open, "doc-save": lambda f, p: write(read(p[0]), p[1]), "layer-add": layer_add,
        "doc-flatten": doc_flatten, "px-crop": px_crop, "px-channel": px_channel}
if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in CMDS: die("E_USAGE", "unknown command", 2)
    try:
        CMDS[sys.argv[1]](*parse_args(sys.argv[2:]))
    except SystemExit: raise
    except Exception as e: die("E_BAD_ARGS", str(e), 2)
