#!/bin/bash
# Verifier entrypoint. Must leave /logs/verifier/reward.json on every path.
mkdir -p /logs/verifier

# Separate-verifier mode: this container never ran the agent. Harbor re-materialises each
# collected artifact at its ORIGINAL path, so /app/src and /app/conformance.json are already
# in place and nothing needs staging here. task.toml excludes build products from those
# artifacts, so /app/src is the agent's SOURCE and the verifier compiles it below, in a
# container the agent never had access to.

chmod 0700 /tests/refldx 2>/dev/null
python3 /tests/verify.py
rc=$?
if [ ! -s /logs/verifier/reward.json ]; then
  echo '{"reward": 0.0, "functional_correctness": 0.0, "constraint_satisfaction": 0.0, "robustness": 0.0, "artifact_quality": 0.0, "gates_passed": 0, "verifier_crash": 1}' > /logs/verifier/reward.json
  echo "verify.py exited $rc without a reward file" > /logs/verifier/crash.txt
fi
exit 0
