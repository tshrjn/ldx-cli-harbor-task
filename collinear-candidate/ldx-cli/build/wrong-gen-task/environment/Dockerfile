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
RUN chmod 0755 /app/refldx /app/compare.sh && mkdir -p /app/src /logs/agent /logs/verifier
