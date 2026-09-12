#!/usr/bin/env bash
# Partial self-check: compares /app/src/ldx with /app/refldx on ordinary invocations over the
# sample assets. Common cases only. 100% here does NOT imply conformance; the grader uses
# different inputs, parameters and behaviours.
set -u
cd "$(dirname "$0")"
REF=./refldx
MINE=./src/ldx
if ! make -s -C src >/dev/null 2>&1 || [ ! -x "$MINE" ]; then
  echo "build failed or $MINE missing (run: make -C src)"; exit 1
fi
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
declare -A pass total

check() {  # check <op> <args...>   (args use IN/OUT placeholders)
  local op=$1; shift
  local args=("$@")
  local ra=("${args[@]/OUT/$T/r.out}") ma=("${args[@]/OUT/$T/m.out}")
  "$REF" "${ra[@]}" >"$T/r.stdout" 2>"$T/r.stderr"; local rrc=$?
  "$MINE" "${ma[@]}" >"$T/m.stdout" 2>"$T/m.stderr"; local mrc=$?
  local ok=1
  [ "$rrc" = "$mrc" ] || ok=0
  cmp -s "$T/r.stdout" "$T/m.stdout" || ok=0
  if [ -f "$T/r.out" ]; then cmp -s "$T/r.out" "$T/m.out" || ok=0; fi
  # compare only the error/warning code on stderr
  diff <(grep -o '"code":"[A-Z_]*"' "$T/r.stderr") <(grep -o '"code":"[A-Z_]*"' "$T/m.stderr") >/dev/null || ok=0
  total[$op]=$(( ${total[$op]:-0} + 1 )); pass[$op]=$(( ${pass[$op]:-0} + ok ))
  rm -f "$T/r.out" "$T/m.out"
}

for f in assets/*.ldx; do
  check doc-open doc-open "$f"
  check doc-open doc-open --dump "$f"
  check doc-save doc-save "$f" OUT
  check layer-add layer-add --name Fresh "$f" OUT
  check layer-add layer-add --name Half --opacity 128 --blend multiply "$f" OUT
  check layer-add layer-add --name Grp --kind group "$f" OUT
  check doc-flatten doc-flatten "$f" OUT
  check px-crop px-crop --x 1 --y 1 --w 3 --h 2 "$f" OUT
  check px-channel px-channel --op invert "$f" OUT
  check px-channel px-channel --op threshold --arg 100 "$f" OUT
done
check doc-new doc-new --w 5 --h 4 --mode rgb OUT
check doc-new doc-new --w 6 --h 3 --mode gray --dpi 300 OUT
check doc-new doc-new --w 4 --h 4 --mode rgb --fill 10,20,30,255 OUT
check layer-add layer-add --name Photo --from assets/gradient_rgb_16x16.ppm assets/blank_rgb_16x16.ldx OUT
check layer-add layer-add --name Shape --from assets/gradient_gray_16x16.pgm --alpha-from assets/mask_16x16.pgm assets/blank_gray_16x16.ldx OUT
check px-channel px-channel --op swap --index 1 assets/photo_rgb_16x16.ldx OUT
check px-channel px-channel --op extract_alpha --index 1 assets/photo_rgb_16x16.ldx OUT

P=0; N=0
for op in doc-new doc-open doc-save layer-add doc-flatten px-crop px-channel; do
  p=${pass[$op]:-0}; n=${total[$op]:-0}; P=$((P+p)); N=$((N+n))
  printf '%-12s %3d/%-3d\n' "$op" "$p" "$n"
done
printf 'overall      %3d/%-3d  (%d%%)\n' "$P" "$N" $(( N ? 100*P/N : 0 ))
echo "note: partial check over sample assets and ordinary parameters only."
