#!/bin/bash
# Evidence for build/SPEC-V2-DISCOVERABILITY.md: every fact SPEC v2 removes, recovered by
# running the reference alone. Usage: spec_v2_probe.sh <path-to-refldx>
L=${1:?usage: spec_v2_probe.sh <refldx>}
W=$(mktemp -d); cd $W || exit 1
echo "## error codes"
$L doc-new --w 0 --h 4 --mode rgb a.ldx        2>&1 >/dev/null | head -1
$L doc-new --w 4 --h 4 --mode indexed a.ldx    2>&1 >/dev/null | head -1
$L doc-open /nonexistent.ldx                   2>&1 >/dev/null | head -1
printf 'not an ldx' > bad.ldx; $L doc-open bad.ldx 2>&1 >/dev/null | head -1
$L doc-new --w 4 --h 4 --mode rgb --bogus 1 a.ldx 2>&1 >/dev/null | head -1
echo "## ranges (boundary bisection)"
$L doc-new --w 2 --h 2 --mode rgb b.ldx
for w in 16384 16385; do $L doc-new --w $w --h 1 --mode gray t.ldx 2>/dev/null && echo "w=$w ok" || echo "w=$w rejected"; done
for o in 255 256;     do $L layer-set --index 0 --opacity $o b.ldx t.ldx 2>/dev/null && echo "opacity=$o ok" || echo "opacity=$o rejected"; done
for d in 65535 65536; do $L doc-new --w 2 --h 2 --mode gray --dpi $d t.ldx 2>/dev/null && echo "dpi=$d ok" || echo "dpi=$d rejected"; done
echo "## container layout"
$L doc-new --w 2 --h 1 --mode rgb --fill 10,20,30,40 h.ldx; xxd h.ldx | head -4
echo "## doc-open schema + defaults"
$L doc-open h.ldx
$L doc-new --w 1 --h 1 --mode gray d.ldx && $L doc-open --dump d.ldx
echo "## blend math"
for b in normal multiply screen darken lighten difference; do
  $L doc-new --w 1 --h 1 --mode rgb --fill 200,60,255,255 base.ldx
  $L layer-add --name top --fill 100,150,50,255 --blend $b base.ldx t.ldx
  $L doc-flatten t.ldx f.ldx
  printf "%-11s %s\n" "$b" "$($L doc-open --dump f.ldx | sed 's/.*"data":"//;s/".*//')"
done
rm -rf $W
