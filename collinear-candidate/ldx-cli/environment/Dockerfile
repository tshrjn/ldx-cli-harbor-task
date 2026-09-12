# Pinned to the linux/amd64 manifest of ubuntu:24.04 by digest. This is the amd64 image
# itself, not the multi-arch index, so the architecture is fixed without a --platform flag
# (which some Dockerfile parsers, E2B's included, cannot read).
FROM ubuntu:24.04@sha256:1e0a86e57d247923571b75e0aaf48a1449cf8c543d51fb3e07a4a7d7bfa79316

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain for the agent (C/C++/Python are all acceptable implementation languages)
# plus the utilities the verifier needs (python3, strace, file).  No image libraries.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc g++ make libc6-dev python3 strace file xxd bsdextrautils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY workspace/ /app/
# The reference is executable by everyone and readable by no one but root.  "Run it, do not
# read it" was an honour-system rule until now: the image ships `xxd` and `file`, the binary
# was 0755, and the agent ran as root, so nothing stopped a reader being pointed at it and
# nothing observed that it had been.  0711 plus a non-root agent turns the rule into a
# permission error while leaving the intended affordance -- running it, without limit --
# completely intact.
# ubuntu:24.04 ships its own `ubuntu` account at uid 1000, so the account has to be freed
# before `agent` can take that uid (useradd would otherwise exit 4, UID already in use).
RUN userdel -r ubuntu \
    && useradd -m -u 1000 agent \
    && chmod 0711 /app/refldx && chmod 0755 /app/compare.sh \
    && mkdir -p /app/src /logs/agent /logs/verifier \
    # Seed the attestation with the blank template. The agent is told to fill it in; if it
    # never does, that must score zero on artifact quality, NOT crash artifact collection
    # and take the whole trial down with it. A harness fault must never be charged to the
    # agent.
    && cp /app/conformance.template.json /app/conformance.json \
    && chown -R agent:agent /app/src /app/conformance.json /logs/agent

USER agent
