# OTTO Docker Images

Security-hardened container images for OTTO API servers.

## Quick Start

```bash
# Build all images
docker build -f docker/Dockerfile.carta -t otto-carta .
docker build -f docker/Dockerfile.velo -t otto-velo .
docker build -f docker/Dockerfile.locus -t otto-locus .
docker build -f docker/Dockerfile.fuelwise -t otto-fuelwise .
docker build -f docker/Dockerfile.ralph -t otto-ralph .

# Run with map data
docker run -p 8081:8081 -v $(pwd)/data:/data:ro otto-carta /data/map.osm.pbf
docker run -p 8082:8082 -v $(pwd)/data:/data:ro otto-velo /data/map.osm.pbf
docker run -p 8083:8083 -v $(pwd)/data:/data:ro otto-locus /data/map.osm.pbf

# Run stateless services
docker run -p 8080:8080 otto-fuelwise
docker run -p 8084:8084 otto-ralph
```

## Images

| Image | Port | Description | Data Required |
|-------|------|-------------|---------------|
| `otto-carta` | 8081 | Map tile server (MVT/PNG) | OSM PBF or .idx |
| `otto-velo` | 8082 | Route server | OSM PBF or .vlg |
| `otto-locus` | 8083 | Geocoder | OSM PBF or .lcx |
| `otto-fuelwise` | 8080 | Refueling optimizer | None (via API) |
| `otto-ralph` | 8084 | LP/MIP solver | None (via API) |

## Security Features

All images use [Google Distroless](https://github.com/GoogleContainerTools/distroless) as the runtime base:

| Feature | Status |
|---------|--------|
| **Base image** | `gcr.io/distroless/cc-debian12:nonroot` |
| **Shell** | None (no /bin/sh, no /bin/bash) |
| **Package manager** | None (no apt, no apk) |
| **User** | nonroot (uid 65532) |
| **Root filesystem** | Can be read-only |
| **Attack surface** | Minimal (~20MB vs ~80MB debian-slim) |

### Running with Read-Only Filesystem

```bash
docker run --read-only \
  -p 8081:8081 \
  -v $(pwd)/data:/data:ro \
  otto-carta /data/map.osm.pbf
```

### Security Options

```bash
docker run \
  --read-only \
  --security-opt=no-new-privileges:true \
  --cap-drop=ALL \
  -p 8081:8081 \
  -v $(pwd)/data:/data:ro \
  otto-carta /data/map.osm.pbf
```

## Docker Compose

See `docker-compose.yml` in the repo root for multi-service deployment:

```bash
# Start GIS trifecta (carta, velo, locus)
docker-compose up carta velo locus

# Start all services
docker-compose up
```

## systemd Integration

For bare-metal or VM deployments, use the systemd service files in `docker/systemd/`:

```bash
# Install service files
sudo cp docker/systemd/otto-base.conf /etc/systemd/system/
sudo cp docker/systemd/*.service /etc/systemd/system/

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable carta-tile-server
sudo systemctl start carta-tile-server
```

See [docs/internals/security-model.md](../docs/internals/security-model.md) for the full security architecture.

## Building for Different Architectures

```bash
# Multi-arch build (requires Docker Buildx)
docker buildx build --platform linux/amd64,linux/arm64 \
  -f docker/Dockerfile.carta -t otto-carta:latest .
```

## Troubleshooting

### No shell for debugging

Distroless images have no shell. For debugging, use the debug variant:

```bash
# Build with debug base (has busybox shell)
# Edit Dockerfile to use: gcr.io/distroless/cc-debian12:debug-nonroot
docker run -it --entrypoint /busybox/sh otto-carta-debug
```

### Health checks

Distroless has no curl/wget. Use external health checks:

```yaml
# docker-compose.yml
services:
  carta:
    healthcheck:
      test: ["CMD-SHELL", "exit 0"]  # No-op, use orchestrator checks
      # Or use a sidecar for health checking
```

For Kubernetes, use HTTP probes:

```yaml
livenessProbe:
  httpGet:
    path: /api/v1/health
    port: 8081
```
