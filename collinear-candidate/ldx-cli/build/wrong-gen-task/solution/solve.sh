#!/bin/bash
# Deliberately wrong generative family, installed as the agent deliverable.
set -euo pipefail
mkdir -p /app/src
cp /solution/*.c /app/src/
cp /solution/Makefile /app/src/Makefile
make -C /app/src
cp /solution/conformance.json /app/conformance.json
echo "wrong-gen: installed"
