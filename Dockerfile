# DEPRECATED - Use per-service Dockerfiles instead
#
# This file is kept for backwards compatibility but will be removed in a future release.
# Nothing builds it: CI, docker-compose and docker/README.md all use
# docker/Dockerfile.fuelwise. It has no named build stages, so `--target` does
# not work against it.
#
# Per-service Dockerfiles (recommended):
#   docker build -f docker/Dockerfile.carta -t otto-carta .
#   docker build -f docker/Dockerfile.velo -t otto-velo .
#   docker build -f docker/Dockerfile.locus -t otto-locus .
#   docker build -f docker/Dockerfile.fuelwise -t otto-fuelwise .
#   docker build -f docker/Dockerfile.ralph -t otto-ralph .
#
# Or use docker-compose:
#   docker-compose up carta velo locus
#
# See docker/README.md for full documentation.
# See docs/internals/security-model.md for security architecture.

# =============================================================================
# This Dockerfile builds FuelWise only (legacy behavior)
# =============================================================================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY ralph/ ralph/
COPY fuelwise/ fuelwise/
COPY shared/ shared/
COPY vendor/ vendor/
COPY Makefile .

RUN make shared && make ralph && make fuelwise && make fuelwise-api

# =============================================================================
# Runtime (distroless)
# =============================================================================
FROM gcr.io/distroless/cc-debian12:nonroot

LABEL org.opencontainers.image.title="OTTO FuelWise API (Legacy Dockerfile)"
LABEL org.opencontainers.image.description="Use docker/Dockerfile.fuelwise instead"

WORKDIR /app

COPY --from=builder --chown=nonroot:nonroot /build/fuelwise/api/fuelwise-api /app/fuelwise-api

EXPOSE 8080

USER nonroot

ENTRYPOINT ["/app/fuelwise-api", "-p", "8080"]
