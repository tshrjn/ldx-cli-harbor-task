#!/usr/bin/env python3
"""Generate sample (visible) and hidden assets by driving the reference binary.

Usage: gen_assets.py --ref <refldx> --visible environment/workspace/assets --hidden tests/hidden_assets
Deterministic for a given --seed.  No expected outputs are produced: assets are *inputs*.
"""
import argparse
import json
import random
import shutil
import subprocess
import tempfile
from pathlib import Path

BLENDS = ["normal", "multiply", "screen", "darken", "lighten", "difference"]


def pnm(path: Path, w: int, h: int, ch: int, rng: random.Random, kind: str = "noise"):
    if kind == "gradient":
        data = bytearray()
        for y in range(h):
            for x in range(w):
                v = [(x * 255) // max(1, w - 1), (y * 255) // max(1, h - 1), ((x + y) * 255) // max(1, w + h - 2)]
                data += bytes(v[:ch])
    elif kind == "disc":  # soft-edged disc, for masks
        data = bytearray()
        cx, cy = (w - 1) / 2, (h - 1) / 2
        r = min(w, h) / 2
        for y in range(h):
            for x in range(w):
                d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
                v = 255 if d < r * 0.6 else (0 if d > r else int(255 * (r - d) / (r * 0.4)))
                data += bytes([max(0, min(255, v))] * ch)
    else:
        data = bytes(rng.randrange(256) for _ in range(w * h * ch))
    magic = b"P6" if ch == 3 else b"P5"
    path.write_bytes(magic + f"\n{w} {h}\n255\n".encode() + bytes(data))


class Gen:
    def __init__(self, ref: Path, rng: random.Random):
        self.ref, self.rng = ref, rng
        self.tmp = Path(tempfile.mkdtemp())
        self.n = 0

    def run(self, *args):
        r = subprocess.run([str(self.ref), *map(str, args)], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"refldx {args} -> {r.returncode}: {r.stderr}")
        return r

    def new(self, out: Path, w, h, mode, alpha=1, dpi=None, fill=None):
        args = ["doc-new", "--w", w, "--h", h, "--mode", mode, "--alpha", alpha]
        if dpi: args += ["--dpi", dpi]
        if fill: args += ["--fill", fill]
        self.run(*args, out)

    def add(self, doc: Path, **kw):
        self.n += 1
        out = self.tmp / f"s{self.n}.ldx"
        args = ["layer-add"]
        for k, v in kw.items():
            args += [f"--{k.replace('_', '-')}", v]
        self.run(*args, doc, out)
        shutil.move(out, doc)

    def rand_fill(self, ch):
        return ",".join(str(self.rng.randrange(256)) for _ in range(ch))


def visible(g: Gen, out: Path, rng: random.Random):
    out.mkdir(parents=True, exist_ok=True)
    pnm(out / "gradient_rgb_16x16.ppm", 16, 16, 3, rng, "gradient")
    pnm(out / "gradient_gray_16x16.pgm", 16, 16, 1, rng, "gradient")
    pnm(out / "mask_16x16.pgm", 16, 16, 1, rng, "disc")
    pnm(out / "noise_rgb_16x16.ppm", 16, 16, 3, rng)
    g.new(out / "blank_rgb_16x16.ldx", 16, 16, "rgb")
    g.new(out / "blank_gray_16x16.ldx", 16, 16, "gray")
    d = out / "photo_rgb_16x16.ldx"
    g.new(d, 16, 16, "rgb")
    g.add(d, name="Photo", **{"from": out / "gradient_rgb_16x16.ppm"}, alpha_from=out / "mask_16x16.pgm")
    g.add(d, name="Tint", fill="30,200,90,180", blend="screen", opacity=200)
    d = out / "groups_rgb_12x12.ldx"
    g.new(d, 12, 12, "rgb", fill="240,240,220,255")
    g.add(d, name="Base", fill="200,40,40,255", opacity=220)
    g.add(d, name="Group A", kind="group", opacity=180)
    g.add(d, name="In A", parent=2, fill="20,20,200,255", opacity=200, blend="multiply")
    g.add(d, name="Group B", kind="group", parent=2, opacity=100)
    g.add(d, name="In B", parent=4, fill="0,255,0,255", blend="difference")
    g.add(d, name="Hidden", parent=2, fill="255,0,255,255", visible=0)
    d = out / "gray_layers_10x7.ldx"
    g.new(d, 10, 7, "gray", fill="40,255")
    g.add(d, name="Mid", fill="200,128", blend="multiply")
    g.add(d, name="Top", fill="90,255", blend="difference", opacity=90, locked=1)
    d = out / "names_pad_gray_5x3.ldx"
    g.new(d, 5, 3, "gray", fill="128,255")
    g.add(d, name="ab", fill="10,200")
    g.add(d, name="abc", fill="20,150")
    g.add(d, name="abcd", fill="30,100")
    d = out / "noalpha_rgb_8x8.ldx"
    g.new(d, 8, 8, "rgb", alpha=0, fill="100,150,200")
    g.add(d, name="Over", fill="250,10,10", opacity=128)
    d = out / "rgb_300dpi_7x5.ldx"
    g.new(d, 7, 5, "rgb", dpi=300, fill="0,0,0,0")
    g.add(d, name="Semi", fill="255,255,255,128", blend="lighten")


def hidden(g: Gen, out: Path, rng: random.Random, count: int):
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    sizes = [(1, 1), (2, 3), (3, 2), (4, 4), (5, 3), (7, 7), (8, 6), (9, 5), (11, 4), (13, 13), (16, 9), (17, 6), (20, 20)]
    for i in range(count):
        w, h = rng.choice(sizes)
        mode = rng.choice(["rgb", "rgb", "gray"])
        alpha = rng.choice([1, 1, 1, 0])
        cc = 3 if mode == "rgb" else 1
        ch = cc + alpha
        name = f"h{i:02d}_{mode}{'a' if alpha else ''}_{w}x{h}.ldx"
        d = out / name
        g.new(d, w, h, mode, alpha=alpha, dpi=rng.choice([72, 96, 300, 1]), fill=g.rand_fill(ch) if rng.random() < 0.7 else None)
        # per-doc PNMs
        pnm_c = out / f"h{i:02d}_{w}x{h}.{'ppm' if cc == 3 else 'pgm'}"
        pnm_a = out / f"h{i:02d}_{w}x{h}_alpha.pgm"
        pnm(pnm_c, w, h, cc, rng, rng.choice(["noise", "gradient"]))
        pnm(pnm_a, w, h, 1, rng, rng.choice(["noise", "disc"]))
        layers = rng.randrange(0, 5)
        groups = []
        idx = 1
        for j in range(layers):
            parent = rng.choice(groups) if groups and rng.random() < 0.5 else -1
            if rng.random() < 0.25:
                g.add(d, name=f"G{j}", kind="group", parent=parent, opacity=rng.randrange(256), visible=rng.choice([1, 1, 1, 0]))
                # index of the new group_open: it is inserted before parent's close; simplest: query doc-open
                info = json.loads(g.run("doc-open", d).stdout)
                groups = [L["index"] for L in info["layers"] if L["kind"] == "group_open"]
            else:
                kw = dict(name=f"L{j}", parent=parent, opacity=rng.randrange(256), blend=rng.choice(BLENDS),
                          visible=rng.choice([1, 1, 1, 0]), locked=rng.choice([0, 0, 1]))
                r = rng.random()
                if r < 0.4:
                    kw["fill"] = g.rand_fill(ch)
                elif r < 0.8:
                    kw["from"] = pnm_c
                    if alpha and rng.random() < 0.7:
                        kw["alpha_from"] = pnm_a
                g.add(d, **kw)
                info = json.loads(g.run("doc-open", d).stdout)
                groups = [L["index"] for L in info["layers"] if L["kind"] == "group_open"]
        manifest.append({"file": name, "w": w, "h": h, "mode": mode, "alpha": alpha,
                         "pnm": pnm_c.name, "alpha_pnm": pnm_a.name})
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--visible", required=True)
    ap.add_argument("--hidden", required=True)
    ap.add_argument("--seed", type=int, default=20260908)
    ap.add_argument("--hidden-count", type=int, default=40)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    g = Gen(Path(a.ref).resolve(), rng)
    for d in (Path(a.visible), Path(a.hidden)):
        if d.exists():
            shutil.rmtree(d)
    visible(g, Path(a.visible), rng)
    hidden(g, Path(a.hidden), rng, a.hidden_count)
    shutil.rmtree(g.tmp)
    print("visible:", sorted(p.name for p in Path(a.visible).iterdir()))
    print("hidden:", len(list(Path(a.hidden).glob('*.ldx'))), "docs")


if __name__ == "__main__":
    main()
