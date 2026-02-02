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

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Index directory
INDEX_DIR="data/index"
mkdir -p "$INDEX_DIR"

# Determine PBF file and region name
if [ -n "$1" ]; then
    if [ -f "$1" ]; then
        PBF_FILE="$1"
        REGION=$(basename "$1" | sed 's/-latest\.osm\.pbf$//' | sed 's/\.osm\.pbf$//' | sed 's/\.pbf$//')
    elif [ -f "data/$1-latest.osm.pbf" ]; then
        PBF_FILE="data/$1-latest.osm.pbf"
        REGION="$1"
    else
        echo -e "${RED}Error: PBF file not found: $1${NC}"
        echo "Available files in data/:"
        ls -1 data/*.pbf 2>/dev/null || echo "  (none)"
        echo ""
        echo "To download OSM data, run:"
        echo "  ./scripts/download-osm.sh monaco    # Small test dataset"
        echo "  ./scripts/download-osm.sh hungary   # Medium dataset"
        exit 1
    fi
else
    PBF_FILE="data/hungary-latest.osm.pbf"
    REGION="hungary"
fi

# Check PBF file exists
if [ ! -f "$PBF_FILE" ]; then
    echo -e "${RED}Error: PBF file not found: $PBF_FILE${NC}"
    echo ""
    echo "To download OSM data, run:"
    echo "  ./scripts/download-osm.sh $REGION"
    exit 1
fi

echo -e "${GREEN}Using region: $REGION${NC}"
echo "  PBF file: $PBF_FILE"
echo ""

# Stop any existing servers
./scripts/stop-demo-servers.sh 2>/dev/null || true
echo ""

# Build all components
echo "Building servers..."
make -j4 carta-api velo-api locus-api >/dev/null 2>&1
echo -e "  ${GREEN}Built: Carta, Velo, Locus${NC}"

# Build WASM demo
echo "Building WASM demo..."
make -C shared/ui/clay-shards-demo >/dev/null 2>&1
echo -e "  ${GREEN}Built: ClayShards demo${NC}"
echo ""

# Define index files
LOCUS_IDX="$INDEX_DIR/$REGION-locus.idx"

# Check for Locus index file
echo "Checking index files..."
if [ -f "$LOCUS_IDX" ]; then
    echo -e "  ${GREEN}Found: $LOCUS_IDX${NC}"
else
    echo -e "  ${YELLOW}Building Locus index (this may take a while)...${NC}"
    # Build index and save it - runs in background
    ./locus/api/locus-geocoder -s "$LOCUS_IDX" "$PBF_FILE" >/dev/null 2>&1 &
    LOCUS_BUILD_PID=$!

    # Wait for index file to be created (poll for file existence)
    echo -n "  "
    while [ ! -f "$LOCUS_IDX" ]; do
        if ! kill -0 $LOCUS_BUILD_PID 2>/dev/null; then
            echo ""
            echo -e "  ${RED}Locus build failed${NC}"
            break
        fi
        printf "."
        sleep 5
    done

    # Index file created - kill the build server
    if [ -f "$LOCUS_IDX" ]; then
        echo ""
        echo -e "  ${GREEN}Built: $LOCUS_IDX${NC}"
        kill $LOCUS_BUILD_PID 2>/dev/null || true
        sleep 1
    else
        echo ""
        echo -e "  ${YELLOW}Index not created, using PBF directly${NC}"
    fi
fi
echo ""

# Start servers in background
echo "Starting servers..."

# Carta tile server (port 8081)
./carta/api/carta-server "$PBF_FILE" >/dev/null 2>&1 &
CARTA_PID=$!
echo "  Started: Carta (http://localhost:8081) [PID: $CARTA_PID]"

# Velo route server (port 8082)
./velo/api/velo-server "$PBF_FILE" >/dev/null 2>&1 &
VELO_PID=$!
echo "  Started: Velo (http://localhost:8082) [PID: $VELO_PID]"

# Locus geocoding server (port 8083) - use index if available
if [ -f "$LOCUS_IDX" ]; then
    ./locus/api/locus-geocoder "$LOCUS_IDX" >/dev/null 2>&1 &
    LOCUS_PID=$!
    echo "  Started: Locus (http://localhost:8083) [PID: $LOCUS_PID] - using binary index"
else
    ./locus/api/locus-geocoder "$PBF_FILE" >/dev/null 2>&1 &
    LOCUS_PID=$!
    echo "  Started: Locus (http://localhost:8083) [PID: $LOCUS_PID] - building from PBF..."
fi

# Start demo HTTP server (port 8000)
# Serve from shared/ui so that relative imports work (demo imports ../clay-shards-webgl)
cd shared/ui
python3 -m http.server 8000 >/dev/null 2>&1 &
DEMO_PID=$!
cd ../..
echo "  Started: Demo (http://localhost:8000/clay-shards-demo/) [PID: $DEMO_PID]"

echo ""
echo "Waiting for servers to be ready..."

# Wait for each server with timeout
wait_for_server() {
    local name=$1
    local url=$2
    local timeout=$3

    for i in $(seq 1 $timeout); do
        if curl -s "$url" >/dev/null 2>&1; then
            echo -e "  ${GREEN}$name ready${NC}"
            return 0
        fi
        sleep 1
    done
    echo -e "  ${YELLOW}$name timeout (may still be loading)${NC}"
    return 1
}

# Wait for servers (Carta and Velo should be quick, Locus may take longer if building from PBF)
wait_for_server "Carta" "http://localhost:8081/api/v1/health" 30
wait_for_server "Velo" "http://localhost:8082/api/v1/health" 60
wait_for_server "Locus" "http://localhost:8083/api/v1/health" 300

echo ""
echo -e "${GREEN}=== All servers running ===${NC}"
echo ""
echo "  Demo:     http://localhost:8000/clay-shards-demo/"
echo "  Carta:    http://localhost:8081"
echo "  Velo:     http://localhost:8082"
echo "  Locus:    http://localhost:8083"
echo ""
echo "Run './scripts/stop-demo-servers.sh' to stop all servers."
