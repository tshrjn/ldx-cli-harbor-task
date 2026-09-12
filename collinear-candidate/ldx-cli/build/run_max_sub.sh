#!/usr/bin/env bash
# Launch target-model trials on a Claude Max subscription (no OpenRouter credits used).
#
# Prerequisite, run once by a human — it opens a browser for OAuth:
#     claude setup-token
# then put the printed token in the repo .env as:
#     CLAUDE_CODE_OAUTH_TOKEN=sk-ant-oat01-...
#
# Harbor forwards CLAUDE_CODE_OAUTH_TOKEN into the container and, with no ANTHROPIC_BASE_URL
# set, strips any provider prefix from -m, so pass the plain Anthropic model id.
set -euo pipefail
cd "$(dirname "$0")/../../.."          # repo root (evo-machines)

TOKEN=$(grep -oE '^CLAUDE_CODE_OAUTH_TOKEN=.*' .env | cut -d= -f2- | xargs)
[ -n "$TOKEN" ] || { echo "CLAUDE_CODE_OAUTH_TOKEN missing from .env — run: claude setup-token" >&2; exit 1; }
export CLAUDE_CODE_OAUTH_TOKEN="$TOKEN"
export E2B_API_KEY=$(grep -oE '^E2B_API_KEY=.*' .env | cut -d= -f2- | xargs)
unset ANTHROPIC_BASE_URL ANTHROPIC_API_KEY   # force the subscription path, not a gateway

MODEL="${MODEL:-claude-opus-4-7}"
# Validate through the Claude Code client, not raw curl: setup-token OAuth tokens are scoped
# to that client and a direct /v1/messages call returns 429 even when the token is healthy.
echo "checking the token and model id via the claude client..."
claude --print --model "$MODEL" "reply with exactly: OK" || {
  echo "token or model rejected by the claude client" >&2; exit 1; }

T=collinear-candidate/ldx-cli
O=$T/build/jobs
harbor run -p ./$T/build/hard-task -a claude-code -m "$MODEL" -e e2b -o ./$O --job-name max-hard-opus47  -y &
harbor run -p ./$T                 -a claude-code -m "$MODEL" -e e2b -o ./$O --job-name max-full-opus47 \
    --agent-timeout-multiplier 0.35 -y &
wait
echo "done — rewards:"
for j in max-hard-opus47 max-full-opus47; do
  echo -n "  $j: "; cat $O/$j/*__*/verifier/reward.json 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin)["reward"])' 2>/dev/null || echo "no reward file"
done
