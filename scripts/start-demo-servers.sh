#!/bin/bash
# Start all demo servers for ClayShards map demo
#
# Ports:
#   8081 - Carta (tile server)
#   8082 - Velo (route server)
#   8083 - Locus (geocoding server)
#   8000 - Demo HTTP server
#
# Usage:
#   ./scripts/start-demo-servers.sh              # Uses data/hungary-latest.osm.pbf
#   ./scripts/start-demo-servers.sh monaco       # Uses data/monaco-latest.osm.pbf
#   ./scripts/start-demo-servers.sh /path/to.pbf # Uses custom PBF file

set -e
cd "$(dirname "$0")/.."

# Determine PBF file
if [ -n "$1" ]; then
    if [ -f "$1" ]; then
        PBF_FILE="$1"
    elif [ -f "data/$1-latest.osm.pbf" ]; then
        PBF_FILE="data/$1-latest.osm.pbf"
    else
        echo "Error: PBF file not found: $1"
        echo "Available files in data/:"
        ls -1 data/*.pbf 2>/dev/null || echo "  (none)"
        exit 1
    fi
else
    PBF_FILE="data/hungary-latest.osm.pbf"
fi

echo "Using PBF file: $PBF_FILE"
echo ""

# Stop any existing servers
./scripts/stop-demo-servers.sh
echo ""

# Build all components
echo "Building servers..."
make -j4 carta-api velo-api locus-api >/dev/null 2>&1
echo "  Built: Carta, Velo, Locus"

# Build WASM demo
echo "Building WASM demo..."
make -C shared/ui/clay-shards-demo >/dev/null 2>&1
echo "  Built: ClayShards demo"
echo ""

# Start servers in background
echo "Starting servers..."

# Carta tile server (port 8081)
./carta/api/carta-server "$PBF_FILE" >/dev/null 2>&1 &
echo "  Started: Carta (http://localhost:8081)"

# Velo route server (port 8082)
./velo/api/velo-server "$PBF_FILE" >/dev/null 2>&1 &
echo "  Started: Velo (http://localhost:8082)"

# Locus geocoding server (port 8083)
./locus/api/locus-geocoder "$PBF_FILE" >/dev/null 2>&1 &
LOCUS_PID=$!
echo "  Started: Locus (http://localhost:8083) - loading index..."

# Start demo HTTP server (port 8000)
cd shared/ui/clay-shards-demo
python3 -m http.server 8000 >/dev/null 2>&1 &
cd ../../..
echo "  Started: Demo (http://localhost:8000)"

echo ""
echo "Waiting for Locus to load index (this may take a while for large PBF files)..."

# Wait for Locus to be ready (check health endpoint)
for i in {1..120}; do
    if curl -s http://localhost:8083/api/v1/health >/dev/null 2>&1; then
        echo "  Locus ready!"
        break
    fi
    if ! kill -0 $LOCUS_PID 2>/dev/null; then
        echo "  Error: Locus failed to start"
        exit 1
    fi
    sleep 2
    printf "."
done
echo ""

echo ""
echo "=== All servers running ==="
echo ""
echo "  Demo:     http://localhost:8000"
echo "  Carta:    http://localhost:8081"
echo "  Velo:     http://localhost:8082"
echo "  Locus:    http://localhost:8083"
echo ""
echo "Run './scripts/stop-demo-servers.sh' to stop all servers."
