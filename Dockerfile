# FuelWise Platform - Multi-stage Dockerfile
#
# Stages:
#   1. c-builder    - Build C libraries and API server
#   2. wasm-builder - Build WebAssembly module (optional)
#   3. ui-builder   - Build React frontend
#   4. runtime      - Final production image

# =============================================================================
# Stage 1: Build C components (ralph, fuelwise, api)
# =============================================================================
FROM debian:bookworm-slim AS c-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy source files
COPY ralph/ ralph/
COPY fuelwise/ fuelwise/
COPY api/ api/
COPY include/ include/
COPY Makefile .

# Build libraries and API
RUN make ralph && \
    make fuelwise && \
    make api

# Run tests to verify build
RUN make test

# =============================================================================
# Stage 2: Build WebAssembly module (optional, for serving from API)
# =============================================================================
FROM emscripten/emsdk:3.1.51 AS wasm-builder

WORKDIR /build

# Copy source files
COPY ralph/ ralph/
COPY fuelwise/ fuelwise/
COPY wasm/ wasm/
COPY include/ include/

# Build WASM
RUN cd wasm && emmake make

# =============================================================================
# Stage 3: Build React UI
# =============================================================================
FROM node:20-slim AS ui-builder

WORKDIR /build/ui

# Copy package files first for better caching
COPY ui/package.json ui/package-lock.json* ./

# Install dependencies
RUN npm ci

# Copy source and build
COPY ui/ .

# Build production bundle
RUN npm run build

# =============================================================================
# Stage 4: Runtime image
# =============================================================================
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    nginx \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy API binary
COPY --from=c-builder /build/api/fuelwise-api /app/fuelwise-api

# Copy WASM files (optional)
COPY --from=wasm-builder /build/wasm/build/ /app/wasm/

# Copy UI build
COPY --from=ui-builder /build/ui/dist/ /var/www/html/

# Copy nginx config
COPY docker/nginx.conf /etc/nginx/sites-available/default

# Copy entrypoint script
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Expose ports
EXPOSE 80 8080

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/api/v1/health || exit 1

# Default command
ENTRYPOINT ["/app/entrypoint.sh"]

# =============================================================================
# Alternative: API-only image
# =============================================================================
FROM debian:bookworm-slim AS api-only

WORKDIR /app

# Copy API binary
COPY --from=c-builder /build/api/fuelwise-api /app/fuelwise-api

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/api/v1/health || exit 1

CMD ["/app/fuelwise-api", "-p", "8080"]

# =============================================================================
# Alternative: Development image with all tools
# =============================================================================
FROM debian:bookworm AS development

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    make \
    curl \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install Node.js
RUN curl -fsSL https://deb.nodesource.com/setup_20.x | bash - && \
    apt-get install -y nodejs

WORKDIR /workspace

# Copy everything
COPY . .

# Build C components
RUN make all

# Install UI dependencies
RUN cd ui && npm install

EXPOSE 5173 8080

CMD ["bash"]
