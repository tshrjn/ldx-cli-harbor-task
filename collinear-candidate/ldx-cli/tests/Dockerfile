# Clean-room verifier image.
#
# In separate-verifier mode Harbor builds THIS directory as the verifier container and never
# reuses the agent's. The agent has root in its own container for four hours; nothing it does
# there can reach this one.
#
# The FROM line and the apt line below are copied verbatim from environment/Dockerfile and
# MUST stay identical to it. If they drift, a deliverable that compiles for the agent could
# fail for the verifier, which would charge a harness fault to the agent.
# build/check_dockerfiles.py enforces this and is run by build/build_refldx.py.

FROM ubuntu:24.04@sha256:1e0a86e57d247923571b75e0aaf48a1449cf8c543d51fb3e07a4a7d7bfa79316

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc g++ make libc6-dev python3 strace file xxd bsdextrautils \
    && rm -rf /var/lib/apt/lists/*

# The verifier and the co-resident reference travel in the image, not as an upload, so the
# agent never has a window in which to observe or modify them.
WORKDIR /app
COPY . /tests/
RUN chmod 0700 /tests/refldx \
    && rm -rf /tests/__pycache__ \
    && mkdir -p /app/src /logs/agent /logs/verifier /logs/artifacts
