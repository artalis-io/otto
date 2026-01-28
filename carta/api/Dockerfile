# Carta Tile Server Dockerfile
#
# Multi-stage build:
# 1. Build C libraries and tile server
# 2. Build React UI
# 3. Create minimal runtime image
#
# Usage:
#   docker build -t carta-tile-server .
#   docker run -p 8081:8081 -v ./data:/data carta-tile-server /data/hungary-latest.osm.pbf

# ============================================================================
# Stage 1: Build C libraries
# ============================================================================
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy source files
COPY carta/ carta/
COPY shared/ shared/
COPY vendor/ vendor/
COPY tile-server/ tile-server/

# Build shared library
WORKDIR /app/shared
RUN make

# Build carta library
WORKDIR /app/carta
RUN make

# Build vendor objects
WORKDIR /app/vendor/miniz
RUN gcc -c -O2 miniz.c miniz_tdef.c miniz_tinfl.c

WORKDIR /app/vendor/mongoose
RUN gcc -c -O2 -D_POSIX_C_SOURCE=200809L mongoose.c

# Build tile server
WORKDIR /app/tile-server
RUN make

# ============================================================================
# Stage 2: Build React UI
# ============================================================================
FROM node:20-slim AS ui-builder

WORKDIR /app/ui

# Copy package files
COPY tile-server/ui/package*.json ./

# Install dependencies
RUN npm ci

# Copy source
COPY tile-server/ui/ ./

# Build for production
RUN npm run build

# ============================================================================
# Stage 3: Runtime image
# ============================================================================
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy tile server binary
COPY --from=builder /app/tile-server/carta-tile-server /app/

# Copy static HTML viewer
COPY --from=builder /app/tile-server/static /app/static/

# Copy React UI build (optional, serves alongside static)
COPY --from=ui-builder /app/ui/dist /app/static/

# Create data directory
RUN mkdir -p /data

# Environment variables
ENV TILE_PORT=8081
ENV TILE_HOST=0.0.0.0
ENV TILE_STATIC_DIR=/app/static
ENV TILE_MIN_ZOOM=0
ENV TILE_MAX_ZOOM=18
ENV TILE_SIZE=512

# Expose port
EXPOSE 8081

# Default command - expects PBF path as argument
ENTRYPOINT ["/app/carta-tile-server", "-s", "/app/static"]
CMD ["/data/map.osm.pbf"]
