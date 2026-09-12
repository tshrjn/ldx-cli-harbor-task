#!/bin/bash
# Oracle solution: installs the reference implementation source as the deliverable.
set -euo pipefail
mkdir -p /app/src
cp /solution/*.c /app/src/
cp /solution/Makefile /app/src/Makefile
make -C /app/src
cp /solution/conformance.json /app/conformance.json
/app/src/ldx doc-new --w 2 --h 2 --mode rgb /tmp/_smoke.ldx && /app/src/ldx doc-open /tmp/_smoke.ldx
echo "oracle: build and smoke test done"
