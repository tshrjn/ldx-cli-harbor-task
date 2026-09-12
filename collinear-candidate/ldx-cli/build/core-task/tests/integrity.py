"""Deliverable integrity checks — anti-smuggling.

The gates in verify.py originally reasoned about the *source tree*: delete byte-identical
copies of the reference, scan for a literal 64-byte slice of it, cap the source size, reject
symlinks.  All four are pattern checks on what the agent shipped, and a pattern check is only
as good as the encodings it anticipates.

A verified exploit defeated every one of them:

    blob.bin  = zlib.compress(open('/app/refldx','rb').read())
    Makefile  = ldx: blob.bin
                    python3 -c "import zlib;open('ldx','wb').write(
                        zlib.decompress(open('blob.bin','rb').read()))"
                    chmod +x ldx

354 KB against a 16 MB cap, no byte-identical copy, no literal probe, and the exec trace sees
`/app/src/ldx` rather than anything called "refldx".  Reward 1.0 for shipping the reference.

The fix is to stop reasoning about the source and reason about the *artifact*, plus close the
smuggling channel that fed it:

  1. `built_is_reference`  — the decisive one.  Whatever the build did, if the binary it
     produced is the reference, that is the reference.  Encoding-independent by construction:
     zlib, xz, base64, XOR, split across ten files — every route has to materialise the same
     bytes to be useful, and this sees them.
  2. `smuggled_reference`  — the source tree decoded through the obvious transports
     (base64/hex/zlib/gzip/bz2/lzma, and one level of nesting) and re-scanned for the probe.
     Catches the payload before it is ever built.
  3. `opaque_blobs`        — a deliverable is source the agent wrote.  A large file that is
     neither text nor a recognised encoding has no legitimate reason to be there, and this is
     what catches an encoding nobody anticipated.

(1) alone stops the known exploit; (2) and (3) are there so the next one does not need a new
patch.  All three are reported individually so a failure says which rule fired and why.
"""
import base64
import binascii
import bz2
import gzip
import hashlib
import lzma
import zlib
from pathlib import Path

BLOB_LIMIT = 64 * 1024          # a non-source file larger than this is not a deliverable
TEXT_SAMPLE = 8192


def _sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def _looks_like_text(data: bytes) -> bool:
    sample = data[:TEXT_SAMPLE]
    if not sample:
        return True
    if b"\x00" in sample:
        return False
    printable = sum(1 for c in sample if 32 <= c < 127 or c in (9, 10, 13))
    return printable / len(sample) > 0.85


def _decodings(data: bytes):
    """Yield plausible decodings of a file, one level deep, then one nested level."""
    yield data
    first = []
    for fn in (zlib.decompress, gzip.decompress, bz2.decompress, lzma.decompress):
        try:
            out = fn(data)
            first.append(out)
            yield out
        except Exception:
            pass
    if _looks_like_text(data):
        stripped = bytes(c for c in data if not chr(c).isspace())
        for fn in (base64.b64decode, binascii.unhexlify):
            try:
                out = fn(stripped)
                if len(out) > 256:
                    first.append(out)
                    yield out
            except Exception:
                pass
    for inner in first:                       # one nested level: base64 of zlib, etc.
        for fn in (zlib.decompress, gzip.decompress, bz2.decompress, lzma.decompress):
            try:
                yield fn(inner)
            except Exception:
                pass


def check(src_dir: Path, built_binary: Path, reference: Path) -> dict:
    """Return a report dict; `violations` empty means the deliverable is clean."""
    ref = reference.read_bytes()
    ref_sha = _sha(ref)
    probe = ref[len(ref) // 2: len(ref) // 2 + 64]
    out = {"built_is_reference": False, "smuggled_reference": [], "opaque_blobs": [], "violations": []}

    # 1. the artifact itself
    if built_binary.exists() and not built_binary.is_symlink():
        blob = built_binary.read_bytes()
        if _sha(blob) == ref_sha:
            out["built_is_reference"] = True
            out["violations"].append(
                "the built binary is byte-identical to the reference: the deliverable IS refldx")

    # 2 and 3. the source tree
    if src_dir.exists():
        for p in sorted(src_dir.rglob("*")):
            if not p.is_file() or p.is_symlink():
                continue
            try:
                data = p.read_bytes()
            except OSError:
                continue
            if p.resolve() == built_binary.resolve():
                continue                       # the artifact is judged by rule 1
            hit = None
            for cand in _decodings(data):
                if probe in cand or _sha(cand) == ref_sha:
                    hit = "byte-identical" if _sha(cand) == ref_sha else "contains reference code"
                    break
            if hit:
                out["smuggled_reference"].append(f"{p} ({hit} after decoding)")
                continue
            if len(data) > BLOB_LIMIT and not _looks_like_text(data):
                out["opaque_blobs"].append(f"{p} ({len(data)} bytes, not text and not a known encoding)")

    if out["smuggled_reference"]:
        out["violations"].append("reference bytes smuggled into the deliverable: "
                                 + "; ".join(out["smuggled_reference"][:3]))
    if out["opaque_blobs"]:
        out["violations"].append("opaque binary blobs in the deliverable, which must be source: "
                                 + "; ".join(out["opaque_blobs"][:3]))
    return out
