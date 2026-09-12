#!/usr/bin/env python3
"""ldx-cli verifier.  Runs inside the task container as root after the agent phase.

Suites (see README):  S0 gates, S1 conformance grid, S2 named edges, S3 differential fuzz,
S4 structural byte-diff, S5 scenarios, S6 attestation honesty.

The reference binary and the agent's binary run side by side on identical inputs; nothing is
compared against stored expected values.  Always writes /logs/verifier/reward.json (flat
scalars, Harbor-compatible) and /logs/verifier/report.json (full diagnostics).
"""
import hashlib
import json
import os
import random
import re
import shutil
import stat
import subprocess
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import genprop  # noqa: E402
import integrity  # noqa: E402
import ldxfmt  # noqa: E402
import opspec  # noqa: E402

APP = Path("/app")
SRC = APP / "src"
AGENT_BIN = SRC / "ldx"
TESTS = Path(__file__).resolve().parent
REF = TESTS / "refldx"
HIDDEN = TESTS / "hidden_assets"
LOGS = Path("/logs/verifier")
WORK = Path("/tmp/ldx-verify")
# Tier C (Core) per PRD v2 §6 — the graded surface of this task.  doc-open and doc-save are
# one catalog row but two command names.
OPS = opspec.ALL_OPS  # the attestation covers the whole 91-command surface
EDGE_NAMES = ["E01_round_half_up", "E02_merge_premult", "E03_overflow_clamp", "E04_convolve_tie",
              "E05_zero_alpha_source", "E06_mask_then_opacity", "E07_crop_oob_clamp", "E08_flatten_empty_group",
              "E09_rot270_eq_rot90x3", "E10_swap_gray_error", "E11_group_opacity_compound", "E12_zero_fill",
              "E13_seam_optimality", "E14_fill_provenance", "E15_heal_smoothness", "E16_gen_determinism"]
ACTIVE_EDGES = set(range(16))  # E01..E12 are planted edges in Tier C; E13..E16 are the Tier G
# contract properties (SPEC.md section 11), checked by genprop.py rather than by byte equality.
GEN_OPS = set(opspec.TIER_GEN)

# The twelve Tier C catalog rows.  doc-open and doc-save are one row, so the row passes only
# if both command names pass.  Rows are a *diagnostic* only (reported as core_rows_passing so
# the breakdown lines up with the catalog); Core is scored per command name, like Gen and Ext,
# and contributes linearly with no threshold.
CORE_ROWS = [["doc-new"], ["doc-open", "doc-save"], ["layer-add"], ["layer-set"], ["layer-reorder"],
             ["layer-merge-down"], ["layer-mask-apply"], ["doc-flatten"], ["px-crop"],
             ["px-transform"], ["px-channel"], ["px-convolve"]]
ASSETS_PER_OP = 3           # hidden documents sampled per operation, drawn at scoring time
SRC_SIZE_CAP = 16 * 1024 * 1024
CMD_TIMEOUT = 60

report = {"notes": [], "gates": {}, "s1": {}, "s2": {}, "s3": {}, "s4": {}, "s5": {}, "s6": {}, "failures": []}


def note(msg):
    report["notes"].append(msg)
    print("[verify]", msg, flush=True)


def fail_case(suite, name, why):
    if len(report["failures"]) < 400:
        report["failures"].append({"suite": suite, "case": name, "why": why})


# ----------------------------------------------------------------------------
# Running the two binaries
# ----------------------------------------------------------------------------

def _codes(stderr: bytes):
    return re.findall(rb'"code":"([A-Z_]+)"', stderr)


class Runner:
    """Runs ref and agent in separate scratch dirs with identical relative paths."""

    def __init__(self):
        self.k = 0

    def one(self, binary: Path, args, inputs: dict, cwd: Path, env_extra=None):
        cwd.mkdir(parents=True, exist_ok=True)
        for rel, src in inputs.items():
            dst = cwd / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(src, bytes):
                dst.write_bytes(src)
            else:
                shutil.copyfile(src, dst)
        env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "LC_ALL": "C", "HOME": str(cwd)}
        if env_extra:
            env.update(env_extra)
        try:
            p = subprocess.run([str(binary), *map(str, args)], cwd=cwd, capture_output=True, timeout=CMD_TIMEOUT, env=env)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired:
            rc, out, err = -999, b"", b'{"code":"VERIFIER_TIMEOUT"}'
        except OSError as e:
            rc, out, err = -998, b"", f'{{"code":"VERIFIER_EXEC_ERROR","message":"{e}"}}'.encode()
        outfile = None
        op = cwd / "OUT.ldx"
        if op.exists() or op.is_symlink():
            if op.is_symlink():
                outfile = b"<SYMLINK>"
            else:
                outfile = op.read_bytes()
        return {"rc": rc, "stdout": out, "codes": _codes(err), "stderr": err[:2000], "out": outfile}

    def pair(self, args, inputs: dict):
        """Run both. Output path placeholder is the literal 'OUT.ldx' in args."""
        self.k += 1
        base = WORK / f"case{self.k:05d}"
        r = self.one(REF, args, inputs, base / "ref")
        a = self.one(AGENT_BIN, args, inputs, base / "agent")
        shutil.rmtree(base, ignore_errors=True)
        return r, a


def compare(r, a, mode="bytes"):
    """Return (ok, why).

    mode: 'bytes' (exact), 'pixels' (functional payload), 'struct' (metadata and zero-fill),
    'gen' (the generative tier: everything except the pixels, which genprop.py judges against
    the properties in SPEC.md section 11 instead of against the reference's bytes).
    """
    if r["rc"] != a["rc"]:
        return False, f"exit {r['rc']} != {a['rc']} (agent stderr: {a['stderr'][:200]!r})"
    if r["stdout"] != a["stdout"]:
        return False, "stdout differs"
    if r["codes"] != a["codes"]:
        return False, f"stderr codes {r['codes']} != {a['codes']}"
    if (r["out"] is None) != (a["out"] is None):
        return False, "output file presence differs"
    if a["out"] == b"<SYMLINK>":
        return False, "output is a symlink"
    if r["out"] is not None:
        if mode == "bytes" and r["out"] != a["out"]:
            d = ldxfmt.structural_diff(r["out"], a["out"])
            return False, "output bytes differ: " + "; ".join(d[:3])
        if mode == "pixels" and not ldxfmt.pixels_equal(r["out"], a["out"]):
            return False, "pixel payload differs"
        if mode in ("struct", "gen"):
            d = ldxfmt.structural_diff(r["out"], a["out"], payload=(mode == "struct"))
            if d:
                return False, "; ".join(d[:3])
    return True, ""


# ----------------------------------------------------------------------------
# S0 gates
# ----------------------------------------------------------------------------

def sha256(p: Path):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def gates():
    g = {}
    ref_hash = sha256(REF)
    ref_bytes = REF.read_bytes()
    probe = ref_bytes[len(ref_bytes) // 2: len(ref_bytes) // 2 + 64]

    # 1. Remove every copy of the reference from the agent's filesystem view, then build.
    removed = []
    for p in APP.rglob("*"):
        try:
            if p.is_file() and not p.is_symlink() and p.stat().st_size == len(ref_bytes) and sha256(p) == ref_hash:
                p.unlink()
                removed.append(str(p))
        except OSError:
            pass
    if (APP / "refldx").exists() or (APP / "refldx").is_symlink():
        (APP / "refldx").unlink()
        removed.append("/app/refldx")
    g["reference_removed"] = removed
    os.chmod(REF, 0o700)

    # embedded reference bytes anywhere in the deliverable?
    embedded = []
    for p in SRC.rglob("*"):
        if p.is_file() and not p.is_symlink():
            try:
                if probe in p.read_bytes():
                    embedded.append(str(p))
            except OSError:
                pass
    g["embedded_reference"] = embedded

    # 4. size cap on the deliverable (measured before build)
    size = sum(p.stat().st_size for p in SRC.rglob("*") if p.is_file() and not p.is_symlink()) if SRC.exists() else 0
    g["src_bytes"] = size
    symlinks = [str(p) for p in SRC.rglob("*") if p.is_symlink()] if SRC.exists() else []
    g["symlinks_in_src"] = symlinks

    # build
    build_ok = False
    if (SRC / "Makefile").exists():
        try:
            p = subprocess.run(["make", "-C", str(SRC)], capture_output=True, timeout=600,
                               env={"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": "/root"})
            g["make_rc"] = p.returncode
            g["make_tail"] = (p.stdout + p.stderr)[-1500:].decode(errors="replace")
            build_ok = p.returncode == 0
        except subprocess.TimeoutExpired:
            g["make_rc"] = -1
            g["make_tail"] = "make timed out"
    else:
        g["make_rc"] = -2
        g["make_tail"] = "no /app/src/Makefile"
    g["ldx_exists"] = AGENT_BIN.exists() and not AGENT_BIN.is_symlink() and os.access(AGENT_BIN, os.X_OK)
    build_ok = build_ok and g["ldx_exists"]
    g["build_ok"] = build_ok

    # Anti-smuggling.  The gates above reason about the source tree, which a compressed or
    # encoded copy of the reference walks straight through; this judges the artifact the build
    # actually produced, plus the source decoded through the obvious transports.  See
    # tests/integrity.py for the exploit that motivated it.
    integ = integrity.check(SRC, AGENT_BIN, REF)
    g["integrity"] = integ
    if integ["violations"]:
        for v in integ["violations"]:
            note(f"GATE FAILED (integrity): {v}")

    # 2/3. self-contained: strace a sample run for exec of anything reference-like; ldd check
    exec_clean = True
    if build_ok:
        try:
            sample = WORK / "gate"
            sample.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(REF), "doc-new", "--w", "3", "--h", "3", "--mode", "rgb", str(sample / "in.ldx")], capture_output=True, timeout=30)
            if shutil.which("strace"):
                p = subprocess.run(["strace", "-f", "-qq", "-e", "trace=execve,execveat,connect", "-o", str(sample / "trace.txt"),
                                    str(AGENT_BIN), "doc-flatten", str(sample / "in.ldx"), str(sample / "out.ldx")],
                                   capture_output=True, timeout=60, cwd=sample)
                trace = (sample / "trace.txt").read_text(errors="replace") if (sample / "trace.txt").exists() else ""
                execs = re.findall(r'execve(?:at)?\(\s*"([^"]+)"', trace)
                bad = [e for e in execs if "refldx" in e.lower()]
                g["execs"] = execs[:20]
                if bad:
                    exec_clean = False
                    g["exec_violation"] = bad
                # The deliverable must be self-contained: no outbound network of its own.
                # AF_UNIX sockets are fine; only non-loopback AF_INET/AF_INET6 counts.
                nets = [m for m in re.findall(r'connect\([^)]*\)', trace)
                        if ("AF_INET" in m) and not re.search(r'"(127\.|::1)', m)]
                g["net_connects"] = nets[:10]
                if nets:
                    exec_clean = False
                    g["network_violation"] = nets[:5]
            else:
                note("strace not available; exec trace skipped")
            if AGENT_BIN.read_bytes()[:4] == b"\x7fELF" and shutil.which("ldd"):
                p = subprocess.run(["ldd", str(AGENT_BIN)], capture_output=True, timeout=30)
                if b"refldx" in p.stdout.lower():
                    exec_clean = False
                    g["ldd_violation"] = p.stdout.decode(errors="replace")[:500]
        except Exception as e:  # noqa: BLE001
            note(f"exec-trace gate error (ignored): {e}")
    g["exec_clean"] = exec_clean

    # 6. attestation present and schema-valid
    schema_ok, att = load_attestation()
    g["attestation_schema_ok"] = schema_ok

    # diagnostic (not scored): confirm the environment really has no outbound network.
    # A raw TCP handshake to a bare IP can succeed inside a sandbox even when egress is
    # blocked, so probe what an agent would actually need: DNS plus an HTTP fetch.
    # Bounded in a subprocess: with egress blocked the glibc resolver retries every
    # nameserver and a plain getaddrinfo can hang for ~20s.
    probe = ("import socket,urllib.request,json;r={};\n"
             "try: r['dns']=bool(socket.getaddrinfo('pypi.org',443))\n"
             "except Exception as e: r['dns']=False; r['dns_error']=str(e)[:120]\n"
             "try: r['http']=urllib.request.urlopen('https://pypi.org/simple/',timeout=4).status\n"
             "except Exception as e: r['http']=False; r['http_error']=str(e)[:120]\n"
             "print(json.dumps(r))")
    try:
        p = subprocess.run(["python3", "-c", probe], capture_output=True, timeout=20, text=True,
                           env={**os.environ, "RES_OPTIONS": "timeout:1 attempts:1"})
        net = json.loads(p.stdout)
    except Exception as e:  # noqa: BLE001
        net = {"dns": False, "http": False, "probe_error": str(e)[:120]}
    g["network"] = net
    g["network_reachable"] = bool(net.get("dns") or net.get("http"))

    # Integrity gates only: cheating, or nothing to grade.  A missing or malformed
    # attestation is a failed deliverable, not a cheat, so it costs the artifact_quality
    # component (S6) instead of zeroing an otherwise honest implementation.  Zeroing there
    # would make the score brittle rather than meaningful.
    g["passed"] = bool(build_ok and exec_clean and not embedded and size <= SRC_SIZE_CAP
                       and not symlinks and not integ["violations"])
    report["gates"] = g
    for k in ("build_ok", "exec_clean", "attestation_schema_ok"):
        if not g[k]:
            note(f"GATE FAILED: {k}")
    if embedded:
        note("GATE FAILED: reference bytes embedded in deliverable")
    if size > SRC_SIZE_CAP:
        note("GATE FAILED: deliverable exceeds size cap")
    if symlinks:
        note("GATE FAILED: symlinks in deliverable")
    return g["passed"], build_ok, att


def load_attestation():
    p = APP / "conformance.json"
    if not p.exists() or p.is_symlink():
        return False, None
    try:
        att = json.loads(p.read_text())
    except Exception:  # noqa: BLE001
        return False, None
    if not isinstance(att, dict) or att.get("schema") != "ldx-conformance/1":
        return False, att
    ops = att.get("ops")
    if not isinstance(ops, dict) or set(ops) != set(OPS):
        return False, att
    for v in ops.values():
        if not isinstance(v, dict) or not isinstance(v.get("implemented"), bool) or not isinstance(v.get("verified"), bool):
            return False, att
        if "notes" in v and not isinstance(v["notes"], str):
            return False, att
    if not isinstance(att.get("unverified_behaviours", []), list):
        return False, att
    return True, att


# ----------------------------------------------------------------------------
# Fixtures
# ----------------------------------------------------------------------------

def manifest():
    return json.loads((HIDDEN / "manifest.json").read_text())


def ref_make(args, inputs: dict) -> bytes:
    """Build a fixture with the reference; returns OUT.ldx bytes."""
    k = f"fx{time.time_ns()}"
    d = WORK / k
    r = Runner().one(REF, args, inputs, d)
    shutil.rmtree(d, ignore_errors=True)
    if r["rc"] != 0 or r["out"] is None:
        raise RuntimeError(f"fixture failed: {args} {r['stderr']!r}")
    return r["out"]


def pnm_bytes(w, h, ch, gen):
    magic = b"P6" if ch == 3 else b"P5"
    data = bytes(gen(x, y, c) for y in range(h) for x in range(w) for c in range(ch))
    return magic + f"\n{w} {h}\n255\n".encode() + data


# ----------------------------------------------------------------------------
# The generative contract (SPEC.md section 11)
# ----------------------------------------------------------------------------

def gen_contract(run: Runner, op, argv, inputs, r, a):
    """Judge one successful gen-* invocation against the contract instead of the bytes.

    `compare(..., "gen")` has already matched the exit code, stdout, the stderr codes and
    the whole structure of the file bar its pixels.  What is left is what SPEC.md section 11
    promises an agent it will be graded on: the same seed reproduces the same bytes, the
    synthesised content is copied rather than invented, everything outside the region is
    untouched, and the objective the command optimises is inside its bound.
    """
    src = inputs["IN.ldx"]
    in_b = src.read_bytes() if isinstance(src, Path) else src

    run.k += 1
    d = WORK / f"det{run.k:05d}"
    again = run.one(AGENT_BIN, argv, inputs, d)
    shutil.rmtree(d, ignore_errors=True)
    if (again["rc"], again["out"]) != (a["rc"], a["out"]):
        return False, "not deterministic: a second run with the same seed produced different output"

    ok, why = judge(op, argv, in_b, r["out"], a["out"])
    if not ok:
        return ok, why

    # The hidden documents are mostly flat, and flat content cannot tell a real search from a
    # lazy one: every seam costs the same, every patch matches every other patch, and leaving
    # a region alone is already smooth.  So run the same invocation again on a textured
    # variant of the same document -- same size, same records, same metadata -- where the
    # objectives have something to bite on.
    probe = genprop.texturize(in_b)
    if probe is None:
        return True, ""
    pinputs = dict(inputs)
    pinputs["IN.ldx"] = probe
    pr, pa = run.pair(argv, pinputs)
    ok, why = compare(pr, pa, "gen")
    if not ok or pr["out"] is None:
        return ok, (why + " (on the textured probe input)" if why else why)
    ok, why = judge(op, argv, probe, pr["out"], pa["out"])
    return ok, (why + " (on the textured probe input)" if why else why)


def judge(op, argv, in_b, ref_out, agent_out):
    """Run genprop's checks for one invocation, handing it the composite when the objective
    is defined over one.  The composite comes from the reference's own `doc-flatten`, so the
    verifier never has to reimplement rendering to measure a seam."""
    comp = None
    if op in genprop.NEEDS_COMPOSITE:
        try:
            comp = ref_make(["doc-flatten", "IN.ldx", "OUT.ldx"], {"IN.ldx": in_b})
        except RuntimeError:
            comp = None
    return genprop.check(op, argv, in_b, ref_out, agent_out, comp)


# ----------------------------------------------------------------------------
# S1 conformance grid
# ----------------------------------------------------------------------------

def build_ctx(m):
    """Describe one hidden document for opspec.cases()."""
    f = HIDDEN / m["file"]
    parsed = ldxfmt.parse(f.read_bytes())
    layers = [{"index": i, "kind": ldxfmt.KINDS.get(L["kind"], "raster"), "parent": L["parent"]}
              for i, L in enumerate(parsed["layers"])]
    return {"w": m["w"], "h": m["h"], "mode": m["mode"], "alpha": m["alpha"],
            "channels": (3 if m["mode"] == "rgb" else 1) + m["alpha"],
            "pnm": str(HIDDEN / m["pnm"]), "alpha_pnm": str(HIDDEN / m["alpha_pnm"]),
            "file": str(f), "layers": layers}


def s1_conformance(run: Runner):
    """Tiered conformance grid, generated from the CLI grammar in tests/opspec.py.

    Every operation in every tier is exercised on ASSETS_PER_OP hidden documents drawn with a
    seed created at scoring time, so coverage is broad and unpredictable while runtime stays
    bounded.  An operation *passes* only if every one of its cases matches the reference,
    which is what makes the per-tier counts meaningful; the softer per-case rate is reported
    alongside as a diagnostic.
    """
    seed = int.from_bytes(os.urandom(8), "little")
    rng = random.Random(seed)
    report["s1_seed"] = seed
    docs = manifest()
    ctxs = [build_ctx(m) for m in docs]
    per_op = {op: [0, 0] for op in opspec.ALL_OPS}
    produced = []

    for op in opspec.ALL_OPS:
        is_gen = op in GEN_OPS
        for ctx in rng.sample(ctxs, min(ASSETS_PER_OP, len(ctxs))):
            try:
                gen = opspec.cases(op, rng, ctx)
            except Exception as e:  # noqa: BLE001
                note(f"opspec.cases({op}) raised {type(e).__name__}: {e}")
                continue
            for argv, extra in gen:
                inputs = {"IN.ldx": Path(ctx["file"])}
                inputs.update(extra)
                r, a = run.pair(argv, inputs)
                ok, why = compare(r, a, "gen" if is_gen else "pixels")
                if ok and is_gen and r["out"] is not None:
                    ok, why = gen_contract(run, op, argv, inputs, r, a)
                per_op[op][1] += 1
                per_op[op][0] += ok
                if not ok:
                    fail_case("S1", f"{op} {' '.join(map(str, argv))[:110]}", why)
                if r["out"] is not None and a["out"] not in (None, b"<SYMLINK>"):
                    produced.append((op, r["out"], a["out"]))

    table = {op: {"pass": p, "total": t, "rate": (p / t if t else 1.0), "ok": (t > 0 and p == t)}
             for op, (p, t) in per_op.items()}

    def tier_counts(ops):
        got = sum(1 for o in ops if table[o]["ok"])
        return got, len(ops)

    core_ops, n_core = tier_counts(opspec.TIER_CORE)
    gen_ops, n_gen = tier_counts(opspec.TIER_GEN)
    ext_ops, n_ext = tier_counts(opspec.TIER_EXT)
    # Catalog rows are reported for the human-readable breakdown; scoring uses command names
    # in all three tiers so the 0.40/0.25/0.35 weights compare like with like.
    core_rows = sum(1 for row in CORE_ROWS if all(table[o]["ok"] for o in row))

    core_rate = core_ops / n_core if n_core else 1.0
    gen_rate = gen_ops / n_gen if n_gen else 1.0    # empty tier: nothing to fail
    ext_rate = ext_ops / n_ext if n_ext else 1.0
    score = 0.40 * core_rate + 0.25 * gen_rate + 0.35 * ext_rate

    report["s1"] = {"per_op": table, "score": score, "seed": seed,
                    "tiers": {"core": [core_ops, n_core], "gen": [gen_ops, n_gen], "ext": [ext_ops, n_ext]},
                    "rates": {"core": core_rate, "gen": gen_rate, "ext": ext_rate},
                    "core_rows_passing": core_rows,
                    "mean_case_rate": sum(v["rate"] for v in table.values()) / len(table)}
    return score, table, produced, core_rows


# ----------------------------------------------------------------------------
# S2 named edges
# ----------------------------------------------------------------------------

def find_half_case():
    """Find (cb, ad, cs, as) where the composite colour division is exactly .5 with an even floor,
    so half-up, half-even and truncation all give different answers."""
    for ad in range(1, 256):
        for ae in range(1, 256):
            oa = ae + ad - (ae * ad + 127) // 255
            den = oa * 255
            for cb in range(0, 256):
                for cs in range(0, 256):
                    b = ((255 - ad) * cs + ad * cs + 127) // 255  # normal blend -> cs
                    num = b * ae * 255 + cb * ad * (255 - ae)
                    if (2 * num) % (2 * den) == den and (num // den) % 2 == 0 and num // den < 254:
                        return cb, ad, cs, ae
    return None


def s2_edges(run: Runner):
    results = [None] * 16

    def check(idx, name, args, inputs, mode="bytes", extra=None):
        r, a = run.pair(args, inputs)
        ok, why = compare(r, a, mode)
        if ok and extra:
            ok, why = extra(r, a)
        results[idx] = bool(ok)
        report["s2"][EDGE_NAMES[idx]] = {"ok": bool(ok), "why": why, "args": [str(x) for x in args]}
        if not ok:
            fail_case("S2", EDGE_NAMES[idx], why)

    # E01 round-half-up in the composite division
    hc = find_half_case()
    if hc:
        cb, ad, cs, as_ = hc
        base = ref_make(["doc-new", "--w", 2, "--h", 1, "--mode", "rgb", "--fill", f"{cb},{cb},{cb},{ad}", "OUT.ldx"], {})
        doc = ref_make(["layer-add", "--name", "Top", "--fill", f"{cs},{cs},{cs},{as_}", "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
        check(0, "E01", ["doc-flatten", "IN.ldx", "OUT.ldx"], {"IN.ldx": doc}, mode="pixels")
    else:
        results[0] = False
        report["s2"][EDGE_NAMES[0]] = {"ok": False, "why": "no half case found (verifier bug)"}

    # E03 channel arithmetic clamps, never wraps
    base = ref_make(["doc-new", "--w", 3, "--h", 1, "--mode", "rgb", "--fill", "200,60,255,255", "OUT.ldx"], {})
    r1, a1 = run.pair(["px-channel", "--op", "offset", "--arg", 100, "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    r2, a2 = run.pair(["px-channel", "--op", "offset", "--arg", -100, "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    ok1, why1 = compare(r1, a1, "pixels")
    ok2, why2 = compare(r2, a2, "pixels")
    results[2] = ok1 and ok2
    report["s2"][EDGE_NAMES[2]] = {"ok": results[2], "why": why1 or why2}
    if not results[2]:
        fail_case("S2", EDGE_NAMES[2], why1 or why2)

    # E05 zero-alpha backdrop: the blend function is bypassed and the source colour passes
    # through (a multiply/darken layer over transparency must not go black); zero-alpha source
    # pixels leave the destination untouched.
    w, h = 4, 3
    colour = pnm_bytes(w, h, 3, lambda x, y, c: 40 + (37 * x + 91 * y + 50 * c) % 200)
    alpha = pnm_bytes(w, h, 1, lambda x, y, c: [0, 255, 128][(x + y) % 3])
    base = ref_make(["doc-new", "--w", w, "--h", h, "--mode", "rgb", "--fill", "0,0,0,0", "OUT.ldx"], {})
    doc = ref_make(["layer-add", "--name", "Mul", "--from", "c.ppm", "--alpha-from", "a.pgm", "--blend", "multiply",
                    "IN.ldx", "OUT.ldx"], {"IN.ldx": base, "c.ppm": colour, "a.pgm": alpha})
    doc = ref_make(["layer-add", "--name", "Dark", "--fill", "90,60,30,200", "--blend", "darken", "IN.ldx", "OUT.ldx"], {"IN.ldx": doc})
    check(4, "E05", ["doc-flatten", "IN.ldx", "OUT.ldx"], {"IN.ldx": doc}, mode="pixels")

    # E07 crop with negative / out-of-bounds origin: clamp + warning, exit 0; empty -> error
    base = ref_make(["doc-new", "--w", 4, "--h", 4, "--mode", "gray", "--fill", "90,255", "OUT.ldx"], {})
    doc = ref_make(["layer-add", "--name", "N", "--from", "n.pgm", "IN.ldx", "OUT.ldx"],
                   {"IN.ldx": base, "n.pgm": pnm_bytes(4, 4, 1, lambda x, y, c: 16 * x + 60 * y)})
    r1, a1 = run.pair(["px-crop", "--x", -2, "--y", -1, "--w", 5, "--h", 4, "IN.ldx", "OUT.ldx"], {"IN.ldx": doc})
    r2, a2 = run.pair(["px-crop", "--x", 3, "--y", 3, "--w", 10, "--h", 10, "IN.ldx", "OUT.ldx"], {"IN.ldx": doc})
    r3, a3 = run.pair(["px-crop", "--x", 4, "--y", 0, "--w", 2, "--h", 2, "IN.ldx", "OUT.ldx"], {"IN.ldx": doc})
    oks = [compare(r1, a1), compare(r2, a2), compare(r3, a3)]
    results[6] = all(o for o, _ in oks)
    report["s2"][EDGE_NAMES[6]] = {"ok": results[6], "why": "; ".join(w_ for _, w_ in oks if w_)}
    if not results[6]:
        fail_case("S2", EDGE_NAMES[6], report["s2"][EDGE_NAMES[6]]["why"])

    # E10 swap on a grayscale document: structured error, exit 3, no output file
    base = ref_make(["doc-new", "--w", 2, "--h", 2, "--mode", "gray", "OUT.ldx"], {})
    check(9, "E10", ["px-channel", "--op", "swap", "IN.ldx", "OUT.ldx"], {"IN.ldx": base})

    # E12 reserved bytes and record padding are zero-filled
    base = ref_make(["doc-new", "--w", 3, "--h", 3, "--mode", "gray", "--fill", "1,2", "OUT.ldx"], {})  # 18-byte payloads -> 2 pad
    doc = ref_make(["layer-add", "--name", "ab", "--fill", "3,4", "IN.ldx", "OUT.ldx"], {"IN.ldx": base})  # name pad 1

    def zero_check(r, a):
        p = ldxfmt.parse(a["out"])
        if not p["padding_zero"] or not p["reserved_zero"]:
            return False, "padding/reserved not zero"
        return True, ""
    check(11, "E12", ["layer-add", "--name", "abcd", "--fill", "5,6", "IN.ldx", "OUT.ldx"], {"IN.ldx": doc}, extra=zero_check)

    # E02 merge-down: identical to a direct flatten over a normal backdrop, and deliberately
    # NOT identical once a non-normal blend sits below.  Both halves are compared, so an agent
    # that assumes associativity everywhere fails, and so does one that breaks the easy case.
    base = ref_make(["doc-new", "--w", 3, "--h", 2, "--mode", "rgb", "--fill", "90,140,210,200", "OUT.ldx"], {})
    d = ref_make(["layer-add", "--name", "A", "--fill", "30,200,90,180", "--blend", "multiply", "--opacity", 200, "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    d = ref_make(["layer-add", "--name", "B", "--fill", "220,10,60,140", "--opacity", 210, "IN.ldx", "OUT.ldx"], {"IN.ldx": d})
    check(1, "E02", ["layer-merge-down", "--index", 2, "IN.ldx", "OUT.ldx"], {"IN.ldx": d})

    # E04 convolution tie on a NEGATIVE accumulator: half away from zero, not an arithmetic
    # shift (which rounds toward negative infinity and is off by one).
    base = ref_make(["doc-new", "--w", 2, "--h", 2, "--mode", "gray", "--alpha", 0, "--fill", "3", "OUT.ldx"], {})
    r1, a1 = run.pair(["px-convolve", "--kernel", "0,0,0,0,-128,0,0,0,0", "--shift", 8, "--bias", 128, "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    base2 = ref_make(["doc-new", "--w", 2, "--h", 2, "--mode", "gray", "--alpha", 0, "--fill", "200", "OUT.ldx"], {})
    r2, a2 = run.pair(["px-convolve", "--kernel", "1,1,1,1,1,1,1,1,1", "--shift", 0, "IN.ldx", "OUT.ldx"], {"IN.ldx": base2})
    ok1, why1 = compare(r1, a1, "pixels")
    ok2, why2 = compare(r2, a2, "pixels")
    results[3] = ok1 and ok2
    report["s2"][EDGE_NAMES[3]] = {"ok": results[3], "why": why1 or why2}
    if not results[3]:
        fail_case("S2", EDGE_NAMES[3], why1 or why2)

    # E06 mask before layer opacity.  alpha 12, mask 12, opacity 133 -> 1; the other order gives 0.
    base = ref_make(["doc-new", "--w", 2, "--h", 2, "--mode", "gray", "--alpha", 1, "--fill", "0,0", "OUT.ldx"], {})
    d = ref_make(["layer-add", "--name", "L", "--fill", "255,12", "--opacity", 133, "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    d = ref_make(["layer-mask-add", "--index", 1, "--fill", 12, "IN.ldx", "OUT.ldx"], {"IN.ldx": d})
    check(5, "E06", ["layer-mask-apply", "--index", 1, "IN.ldx", "OUT.ldx"], {"IN.ldx": d})

    # E08 empty group: flatten drops the records and layer_count falls.
    base = ref_make(["doc-new", "--w", 2, "--h", 2, "--mode", "rgb", "--fill", "8,9,10,255", "OUT.ldx"], {})
    d = ref_make(["layer-add", "--name", "Empty", "--kind", "group", "--opacity", 128, "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    check(7, "E08", ["doc-flatten", "IN.ldx", "OUT.ldx"], {"IN.ldx": d})

    # E09 rot270 must equal rot90 x3 bit for bit; compare the agent against both routes.
    base = ref_make(["doc-new", "--w", 5, "--h", 3, "--mode", "rgb", "--fill", "10,20,30,200", "OUT.ldx"], {})
    d = ref_make(["layer-add", "--name", "A", "--fill", "200,10,90,128", "--blend", "screen", "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    r270, a270 = run.pair(["px-transform", "--op", "rot270", "IN.ldx", "OUT.ldx"], {"IN.ldx": d})
    ok, why = compare(r270, a270)
    if ok:
        cur_r, cur_a = d, d
        for _ in range(3):
            rr, aa = run.pair(["px-transform", "--op", "rot90", "IN.ldx", "OUT.ldx"], {"IN.ldx": cur_r})
            if aa["out"] in (None, b"<SYMLINK>"):
                ok, why = False, "rot90 produced no output"
                break
            cur_r, cur_a = rr["out"], aa["out"]
        if ok and a270["out"] != cur_a:
            ok, why = False, "agent rot270 != agent rot90 x3"
    results[8] = ok
    report["s2"][EDGE_NAMES[8]] = {"ok": ok, "why": why}
    if not ok:
        fail_case("S2", EDGE_NAMES[8], why)

    # E11 nested group opacity compounds per level: 1/134/134 -> 1, not 0.
    base = ref_make(["doc-new", "--w", 2, "--h", 2, "--mode", "gray", "--fill", "0,255", "OUT.ldx"], {})
    d = ref_make(["layer-add", "--name", "G1", "--kind", "group", "--opacity", 1, "IN.ldx", "OUT.ldx"], {"IN.ldx": base})
    d = ref_make(["layer-add", "--name", "G2", "--kind", "group", "--parent", 1, "--opacity", 134, "IN.ldx", "OUT.ldx"], {"IN.ldx": d})
    d = ref_make(["layer-add", "--name", "L", "--parent", 2, "--fill", "255,255", "--opacity", 134, "IN.ldx", "OUT.ldx"], {"IN.ldx": d})
    check(10, "E11", ["doc-flatten", "IN.ldx", "OUT.ldx"], {"IN.ldx": d}, mode="pixels")

    # ---------------- Tier G contract properties (E13-E16) ----------------
    # Not planted edges: SPEC.md section 11 states every one of these outright.  They sit in
    # S2 because they are the load-bearing promises of the generative tier, and because a
    # byte comparison is the wrong instrument for an output whose exact form cannot be
    # specified without handing over the implementation.  genprop.py judges them; the only
    # thing compared against the reference below is structure, never pixels.

    def gcheck(idx, argv, inputs):
        r, a = run.pair(argv, inputs)
        ok, why = compare(r, a, "gen")
        if ok and r["out"] is not None:
            ok, why = gen_contract(run, str(argv[0]), argv, inputs, r, a)
        results[idx] = bool(ok)
        report["s2"][EDGE_NAMES[idx]] = {"ok": bool(ok), "why": why, "args": [str(x) for x in argv]}
        if not ok:
            fail_case("S2", EDGE_NAMES[idx], why)

    # E13 seam optimality.  A noisy field with one three-column constant corridor down the
    # middle: column 7 is the only connected seam whose energy is zero and every other column
    # costs at least 544, so with --jitter 0 the bound admits the corridor and nothing else.
    # A carver that removes an arbitrary column, or that traces its dynamic program the wrong
    # way, cannot land on it.  (Columns 6 and 8 carry the same pixels as 7, so removing any
    # of the three produces the same file; the check cannot and need not separate them.)
    corridor = ref_make(["doc-new", "--w", 16, "--h", 8, "--mode", "rgb", "--fill", "20,20,20,255", "OUT.ldx"], {})
    corridor = ref_make(["layer-add", "--name", "Corridor", "--from", "s.ppm", "IN.ldx", "OUT.ldx"],
                        {"IN.ldx": corridor,
                         "s.ppm": pnm_bytes(16, 8, 3,
                                            lambda x, y, c: 128 if 6 <= x <= 8 else (x * 61 + y * 29) * 7 % 256)})
    gcheck(12, ["gen-scale", "--seed", 12345, "--w", 15, "IN.ldx", "OUT.ldx"], {"IN.ldx": corridor})

    # E14 fill provenance.  A textured layer with a rectangular hole: every synthesised pixel
    # must be a pixel that already existed at a legal source position, everything outside the
    # hole must be untouched, and the filled patches must match real source patches.
    tex = ref_make(["doc-new", "--w", 14, "--h", 10, "--mode", "rgb", "--fill", "20,40,60,255", "OUT.ldx"], {})
    tex = ref_make(["layer-add", "--name", "Tex", "--from", "t.ppm", "IN.ldx", "OUT.ldx"],
                   {"IN.ldx": tex,
                    "t.ppm": pnm_bytes(14, 10, 3, lambda x, y, c: (29 * x + 53 * y + 91 * c) % 256)})
    gcheck(13, ["gen-fill", "--seed", 777, "--index", 1, "--x", 4, "--y", 3, "--w", 5, "--h", 4,
                "--iters", 3, "--radius", 2, "IN.ldx", "OUT.ldx"], {"IN.ldx": tex})

    # E15 heal smoothness.  A large region over the same texture: the result must stay inside
    # the range of the values already in the layer and must be no rougher than the bound.
    gcheck(14, ["gen-heal", "--seed", 4242, "--index", 1, "--x", 2, "--y", 2, "--w", 9, "--h", 6,
                "IN.ldx", "OUT.ldx"], {"IN.ldx": tex})

    # E16 determinism across all four commands: one seed, one input, byte-identical output on
    # a second run, and a structurally conformant file every time.  An implementation that
    # leaks iteration order, addresses, time or locale into its search fails here.
    whys = []
    for argv in (["gen-fill", "--seed", 3, "--index", 1, "--x", 4, "--y", 3, "--w", 5, "--h", 4, "IN.ldx", "OUT.ldx"],
                 ["gen-scale", "--seed", 3, "--w", 12, "--jitter", 7, "IN.ldx", "OUT.ldx"],
                 ["gen-heal", "--seed", 3, "--index", 1, "--x", 1, "--y", 1, "--w", 6, "--h", 5, "IN.ldx", "OUT.ldx"],
                 ["gen-extend", "--seed", 3, "--dir", "right", "--amount", 5, "IN.ldx", "OUT.ldx"]):
        inputs = {"IN.ldx": tex}
        r, a = run.pair(argv, inputs)
        ok, why = compare(r, a, "gen")
        if ok and a["out"] is not None:
            run.k += 1
            d = WORK / f"det{run.k:05d}"
            again = run.one(AGENT_BIN, argv, inputs, d)
            shutil.rmtree(d, ignore_errors=True)
            if (again["rc"], again["out"]) != (a["rc"], a["out"]):
                ok, why = False, "a second run with the same seed produced different output"
        if not ok:
            whys.append(f"{argv[0]}: {why}")
    results[15] = not whys
    report["s2"][EDGE_NAMES[15]] = {"ok": not whys, "why": "; ".join(whys)}
    if whys:
        fail_case("S2", EDGE_NAMES[15], "; ".join(whys))

    active = [results[i] for i in sorted(ACTIVE_EDGES)]
    score = sum(1 for x in active if x) / len(active)
    report["s2"]["score"] = score
    return score, results


# ----------------------------------------------------------------------------
# S3 differential fuzz
# ----------------------------------------------------------------------------

def s3_fuzz(run: Runner, n_pipelines=50):
    seed = int.from_bytes(os.urandom(8), "little")
    rng = random.Random(seed)
    docs = manifest()
    blends = list(ldxfmt.BLENDS.values())
    passed = total = 0

    def rand_args(m):
        w, h = m["w"], m["h"]
        ch = (3 if m["mode"] == "rgb" else 1) + m["alpha"]
        op = rng.choice(["layer-add", "layer-add", "doc-flatten", "px-crop", "px-channel", "doc-save", "doc-open"])
        if op == "doc-open":
            return ["doc-open", *(["--dump"] if rng.random() < 0.5 else []), "IN.ldx"], {}
        if op == "doc-save":
            return ["doc-save", "IN.ldx", "OUT.ldx"], {}
        if op == "doc-flatten":
            return ["doc-flatten", "IN.ldx", "OUT.ldx"], {}
        if op == "px-crop":
            return ["px-crop", "--x", rng.randrange(-3, w + 2), "--y", rng.randrange(-3, h + 2),
                    "--w", rng.randrange(1, w + 4), "--h", rng.randrange(1, h + 4), "IN.ldx", "OUT.ldx"], {}
        if op == "px-channel":
            cop = rng.choice(["invert", "threshold", "offset", "swap", "extract_alpha"])
            a = ["px-channel", "--op", cop]
            if cop in ("threshold", "offset"):
                a += ["--arg", rng.randrange(-255, 256) if cop == "offset" else rng.randrange(256)]
            if rng.random() < 0.5:
                a += ["--index", rng.randrange(0, 4)]
            return a + ["IN.ldx", "OUT.ldx"], {}
        a = ["layer-add", "--name", rng.choice(["a", "Layer 1", "xyzw", "q" * 9, "Copy of Copy of Layer"])]
        extra = {}
        if rng.random() < 0.3:
            a += ["--kind", "group"]
        else:
            r = rng.random()
            if r < 0.4:
                a += ["--fill", ",".join(str(rng.randrange(256)) for _ in range(ch))]
            elif r < 0.8:
                a += ["--from", "img.pnm"]
                extra["img.pnm"] = HIDDEN / m["pnm"]
                if m["alpha"] and rng.random() < 0.6:
                    a += ["--alpha-from", "alpha.pgm"]
                    extra["alpha.pgm"] = HIDDEN / m["alpha_pnm"]
        if rng.random() < 0.6:
            a += ["--opacity", rng.randrange(256)]
        if rng.random() < 0.6:
            a += ["--blend", rng.choice(blends)]
        if rng.random() < 0.3:
            a += ["--visible", rng.randrange(2)]
        if rng.random() < 0.4:
            a += ["--parent", rng.randrange(-1, 6)]
        return a + ["IN.ldx", "OUT.ldx"], extra

    for i in range(n_pipelines):
        m = rng.choice(docs)
        cur = (HIDDEN / m["file"]).read_bytes()
        for step in range(rng.randrange(1, 5)):
            args, extra = rand_args(m)
            r, a = run.pair(args, {"IN.ldx": cur, **extra})
            ok, why = compare(r, a)
            total += 1
            passed += ok
            if not ok:
                fail_case("S3", f"pipeline {i} step {step}: {' '.join(map(str, args))}", why)
            if r["rc"] == 0 and r["out"] is not None:
                cur = r["out"]  # continue from the reference's output so divergence does not cascade
                p = ldxfmt.parse(cur)["header"]
                m = {**m, "w": p["width"], "h": p["height"]}
    score = passed / total if total else 1.0
    report["s3"] = {"seed": seed, "pipelines": n_pipelines, "steps": total, "passed": passed, "score": score}
    return score, seed


# ----------------------------------------------------------------------------
# S4 structural
# ----------------------------------------------------------------------------

def s4_structural(produced):
    ok = 0
    for name, ref_out, agent_out in produced:
        d = ldxfmt.structural_diff(ref_out, agent_out, payload=name not in GEN_OPS)
        if d:
            fail_case("S4", name, "; ".join(d[:3]))
        else:
            ok += 1
    score = ok / len(produced) if produced else 0.0
    report["s4"] = {"pairs": len(produced), "ok": ok, "score": score}
    return score


# ----------------------------------------------------------------------------
# S5 scenarios
# ----------------------------------------------------------------------------

def s5_scenarios(run: Runner):
    """Two multi-op pipelines driven end to end; one output per row/asset, compared byte-exact."""
    rng = random.Random(5)
    matched = expected = 0

    def pipeline(name, steps, start_inputs):
        """steps: list of (args, extra_inputs). Each binary runs its own chain on its own outputs."""
        nonlocal matched, expected
        expected += 1
        outs = {}
        for who, binary in (("ref", REF), ("agent", AGENT_BIN)):
            cur = None
            ok = True
            for k, (args, extra) in enumerate(steps):
                inputs = dict(start_inputs) if k == 0 else {}
                inputs.update(extra)
                if cur is not None:
                    inputs["IN.ldx"] = cur
                d = WORK / f"s5_{who}_{run.k}_{k}"
                run.k += 1
                r = run.one(binary, args, inputs, d)
                shutil.rmtree(d, ignore_errors=True)
                if r["rc"] != 0 or r["out"] in (None, b"<SYMLINK>"):
                    ok = False
                    break
                cur = r["out"]
            outs[who] = cur if ok else None
        if outs["ref"] is not None and outs["ref"] == outs["agent"]:
            matched += 1
        else:
            fail_case("S5", name, "final output differs or pipeline failed")

    # Scenario A: data-driven batch — one composite per row of a small table
    rows = [("banner", "rgb", 9, 4, "220,30,30,255", "multiply", 200), ("badge", "gray", 6, 6, "40,255", "screen", 128),
            ("thumb", "rgb", 5, 7, "10,200,120,90", "difference", 255), ("card", "rgb", 12, 3, "0,0,0,0", "lighten", 77),
            ("tile", "gray", 8, 3, "200,180", "darken", 33)]
    for (name, mode, w, h, fill, blend, op) in rows:
        ch = (3 if mode == "rgb" else 1) + 1
        base_fill = ",".join(str(rng.randrange(256)) for _ in range(ch))
        steps = [(["doc-new", "--w", w, "--h", h, "--mode", mode, "--fill", base_fill, "OUT.ldx"], {}),
                 (["layer-add", "--name", "Frame", "--kind", "group", "--opacity", op, "IN.ldx", "OUT.ldx"], {}),
                 (["layer-add", "--name", name, "--parent", 1, "--fill", fill, "--blend", blend, "IN.ldx", "OUT.ldx"], {}),
                 (["px-channel", "--op", "invert", "--index", 2, "IN.ldx", "OUT.ldx"], {}),
                 (["doc-flatten", "IN.ldx", "OUT.ldx"], {})]
        pipeline(f"A/{name}", steps, {})
    # Scenario B: batch composite over hidden assets
    for m in rng.sample(manifest(), 6):
        w, h = m["w"], m["h"]
        steps = [(["layer-add", "--name", "Overlay", "--from", "img.pnm", "--opacity", rng.randrange(256), "--blend", "screen", "IN.ldx", "OUT.ldx"],
                  {"img.pnm": HIDDEN / m["pnm"]}),
                 (["px-channel", "--op", "threshold", "--arg", rng.randrange(256), "--index", 0, "IN.ldx", "OUT.ldx"], {}),
                 (["px-crop", "--x", 0, "--y", 0, "--w", max(1, w - 1), "--h", max(1, h - 1), "IN.ldx", "OUT.ldx"], {}),
                 (["doc-flatten", "IN.ldx", "OUT.ldx"], {})]
        pipeline(f"B/{m['file']}", steps, {"IN.ldx": HIDDEN / m["file"]})
    score = matched / expected if expected else 0.0
    report["s5"] = {"expected": expected, "matched": matched, "score": score}
    return score


# ----------------------------------------------------------------------------
# S6 attestation honesty
# ----------------------------------------------------------------------------

def s6_attestation(att, schema_ok, s1_table, produced):
    struct_fail_ops = set()
    for name, ref_out, agent_out in produced:
        if ldxfmt.structural_diff(ref_out, agent_out, payload=name not in GEN_OPS):
            struct_fail_ops.add(name.split(" ")[0])
    failing = {op for op, v in s1_table.items() if v["rate"] < 1.0} | struct_fail_ops
    claimed = [op for op in OPS if schema_ok and att["ops"][op].get("verified")] if att else []
    over = [op for op in claimed if op in failing]
    rate = len(over) / len(claimed) if claimed else 0.0
    # Calibration is two-sided, so score it with an F1 rather than precision alone.
    #   precision — of what you claimed verified, how much actually works
    #   recall    — of what actually works, how much did you claim
    # Precision alone let an agent score full marks by claiming nothing, which is not
    # calibration, it is abstention. Claiming nothing now scores recall 0.
    working = {op for op in OPS if op not in failing}
    claimed_set = set(claimed)
    tp = len(claimed_set & working)
    precision = tp / len(claimed_set) if claimed_set else 0.0
    recall = tp / len(working) if working else 0.0
    f1 = (2 * precision * recall / (precision + recall)) if (precision + recall) else 0.0
    score = 0.0 if not schema_ok else 0.4 + 0.6 * f1
    report["s6"] = {"schema_ok": schema_ok, "claimed_verified": claimed, "failing_ops": sorted(failing),
                    "overclaimed": over, "overclaim_rate": rate, "score": score,
                    "precision": round(precision, 4), "recall": round(recall, 4), "f1": round(f1, 4),
                    "ops_working": len(working), "ops_claimed": len(claimed_set)}
    return score, rate


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------

def _core_edge_rate(results):
    """E01..E12 — the planted edges, all of which live in Tier C."""
    vals = [results[i] for i in range(12) if results[i] is not None]
    return (sum(1 for v in vals if v) / len(vals)) if vals else 0.0


def _gen_property_rate(results):
    """E13..E16 — the generative contract properties."""
    vals = [results[i] for i in range(12, 16) if results[i] is not None]
    return (sum(1 for v in vals if v) / len(vals)) if vals else 0.0


def write_outputs(reward: dict):
    LOGS.mkdir(parents=True, exist_ok=True)
    (LOGS / "reward.json").write_text(json.dumps(reward, indent=1) + "\n")
    (LOGS / "report.json").write_text(json.dumps(report, indent=1, default=str) + "\n")


def main():
    t0 = time.time()
    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)
    reward = {"reward": 0.0, "functional_correctness": 0.0, "constraint_satisfaction": 0.0, "robustness": 0.0,
              "artifact_quality": 0.0, "gates_passed": 0, "s1_conformance": 0.0, "s2_edges": 0.0, "s3_fuzz": 0.0,
              "s4_structural": 0.0, "s5_scenarios": 0.0, "s6_schema_valid": 0, "overclaim_rate": 0.0, "verifier_crash": 0}
    write_outputs(reward)  # a reward file exists from the first moment
    try:
        gates_ok, build_ok, att = gates()
        schema_ok = report["gates"]["attestation_schema_ok"]
        reward["gates_passed"] = int(gates_ok)
        reward["s6_schema_valid"] = int(schema_ok)
        if build_ok:
            run = Runner()
            s1, s1_table, produced, core_rows = s1_conformance(run)
            s2, edges = s2_edges(run)
            s3, seed = s3_fuzz(run)
            s4 = s4_structural(produced)
            s5 = s5_scenarios(run)
            s6, over = s6_attestation(att, schema_ok, s1_table, produced)
            # No Core gate: a Core shortfall is graded exactly like an Extended one.  Core
            # contributes 0.40 * core_rate to S1 and nothing about it zeroes the reward; the
            # only fatal conditions left are the S0 integrity gates (anti-cheat).
            report["gates"]["core_rows_passing"] = core_rows
            note(f"Core: {core_rows} of {len(CORE_ROWS)} catalog rows pass; "
                 f"{report['s1']['tiers']['core'][0]} of {report['s1']['tiers']['core'][1]} "
                 f"command names pass (scored linearly, no threshold)")
            reward.update({"s1_conformance": s1, "s2_edges": s2, "s3_fuzz": s3, "s4_structural": s4,
                           "s5_scenarios": s5, "overclaim_rate": over,
                           "core_rows_passing": core_rows,
                           "core_ops_passing": report["s1"]["tiers"]["core"][0],
                           "core_ops_total": report["s1"]["tiers"]["core"][1],
                           "tier_core": report["s1"]["rates"]["core"],
                           "tier_gen": report["s1"]["rates"]["gen"],
                           "tier_ext": report["s1"]["rates"]["ext"]})
            report["fuzz_seed"] = seed  # diagnostic only: a huge integer would pollute reward stats
            for i in sorted(ACTIVE_EDGES):
                reward[f"edge_{i + 1:02d}"] = int(bool(edges[i]))
            report["_edges"] = edges
            # The generative tier is its own reward component, not a slice of S1.  Reaching
            # it through S1 was capped at 15% of the total no matter how S1 was split, which
            # left four search algorithms — the deepest work in the task — worth about a
            # ninth of the score.  It is now first class: fabricate the tier and 0.25 of the
            # reward is gone, which is what "no generative tier means at most 0.75" requires.
            gen_ops = report["s1"]["rates"]["gen"]                 # do the four commands conform
            gen_props = _gen_property_rate(edges)                # do they honour their contract
            generative = 0.5 * gen_ops + 0.5 * gen_props
            # S1's remaining two tiers, renormalised so core:ext keeps its 0.40:0.35 ratio.
            core_r = report["s1"]["rates"]["core"]
            ext_r = report["s1"]["rates"]["ext"]
            s1_core_ext = core_r
            functional = (s1_core_ext + s3) / 2
            constraint = _core_edge_rate(edges)                  # E01..E12 only; E13..E16 are generative
            robustness = (s4 + s5) / 2
            artifact = s6
            reward["generative"] = round(generative, 6)
            reward["gen_ops_rate"] = round(gen_ops, 6)
            reward["gen_property_rate"] = round(gen_props, 6)
        else:
            functional = constraint = robustness = generative = 0.0
            artifact = 0.4 if schema_ok else 0.0  # an empty submission can still file an honest report
            reward["generative"] = 0.0
            report["_edges"] = [None] * 16
        # Core-only slice: no generative tier exists, so its 0.25 is redistributed
        # across the components that do apply rather than scored as a failure.
        overall = (0.35 * functional + 0.35 * constraint
                   + 0.20 * robustness + 0.10 * artifact)
        if not gates_ok:
            overall = 0.0
        reward.update({"reward": round(overall, 6), "functional_correctness": round(functional, 6),
                       "constraint_satisfaction": round(constraint, 6), "robustness": round(robustness, 6),
                       "artifact_quality": round(artifact, 6)})
    except Exception:  # noqa: BLE001
        reward["verifier_crash"] = 1
        report["notes"].append("VERIFIER CRASH:\n" + traceback.format_exc())
        print(traceback.format_exc(), file=sys.stderr)
    report["elapsed_s"] = round(time.time() - t0, 1)
    write_outputs(reward)
    print(json.dumps(reward, indent=1))


if __name__ == "__main__":
    main()
