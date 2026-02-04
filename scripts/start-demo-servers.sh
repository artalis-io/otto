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
make -C clayshards/clay-shards-demo >/dev/null 2>&1
echo -e "  ${GREEN}Built: ClayShards demo${NC}"
echo ""

# Define index files
LOCUS_IDX="$INDEX_DIR/$REGION-locus.idx"
CARTA_IDX="$INDEX_DIR/$REGION-carta.idx"

# Cross-platform file size helper (works on macOS and Linux)
get_file_size() {
    local file="$1"
    if [ -f "$file" ]; then
        # Try GNU stat first (Linux), then BSD stat (macOS)
        stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

# Helper: check if index file is valid (exists and non-empty)
# Minimum size: 1KB to catch truncated files
is_valid_index() {
    local file="$1"
    local min_size="${2:-1024}"  # Default 1KB minimum
    if [ -f "$file" ]; then
        local size=$(get_file_size "$file")
        if [ "$size" -ge "$min_size" ]; then
            return 0  # Valid
        fi
    fi
    return 1  # Invalid or missing
}

# Check for index files
# NOTE: Carta and Locus indexes are INDEPENDENT - each is checked separately.
# Deleting one index will NOT cause the other to be rebuilt.
echo "Checking index files..."

# Check for Carta index (minimum 1MB for valid index)
# This block is independent of Locus index status
if is_valid_index "$CARTA_IDX" 1048576; then
    CARTA_SIZE=$(get_file_size "$CARTA_IDX")
    echo -e "  ${GREEN}Found: $CARTA_IDX ($(numfmt --to=iec-i --suffix=B $CARTA_SIZE 2>/dev/null || echo "${CARTA_SIZE} bytes"))${NC}"
else
    # Remove invalid/truncated file if it exists
    [ -f "$CARTA_IDX" ] && rm -f "$CARTA_IDX"
    echo -e "  ${YELLOW}Building Carta index (this may take a while)...${NC}"
    ./carta/api/carta-tile-server --lod default --save-index "$CARTA_IDX" "$PBF_FILE" >/dev/null 2>&1 &
    CARTA_BUILD_PID=$!

    echo -n "  "
    while [ ! -f "$CARTA_IDX" ]; do
        if ! kill -0 $CARTA_BUILD_PID 2>/dev/null; then
            echo ""
            echo -e "  ${RED}Carta build failed${NC}"
            break
        fi
        printf "."
        sleep 5
    done

    if [ -f "$CARTA_IDX" ]; then
        # Wait for file size to stabilize (write complete)
        echo ""
        echo -n "  Writing"
        PREV_SIZE=0
        STABLE_COUNT=0
        while [ $STABLE_COUNT -lt 3 ]; do
            if ! kill -0 $CARTA_BUILD_PID 2>/dev/null; then
                # Process exited - file should be complete
                break
            fi
            CURR_SIZE=$(get_file_size "$CARTA_IDX")
            if [ "$CURR_SIZE" = "$PREV_SIZE" ]; then
                STABLE_COUNT=$((STABLE_COUNT + 1))
            else
                STABLE_COUNT=0
                PREV_SIZE=$CURR_SIZE
            fi
            printf "."
            sleep 2
        done
        echo ""
        echo -e "  ${GREEN}Built: $CARTA_IDX ($(numfmt --to=iec-i --suffix=B $CURR_SIZE 2>/dev/null || echo "${CURR_SIZE} bytes"))${NC}"
        kill $CARTA_BUILD_PID 2>/dev/null || true
        sleep 1
    else
        echo ""
        echo -e "  ${YELLOW}Carta index not created, using PBF directly${NC}"
        CARTA_IDX=""
    fi
fi

# Check for Locus index (minimum 100KB for valid index)
# This block is independent of Carta index status
if is_valid_index "$LOCUS_IDX" 102400; then
    LOCUS_SIZE=$(get_file_size "$LOCUS_IDX")
    echo -e "  ${GREEN}Found: $LOCUS_IDX ($(numfmt --to=iec-i --suffix=B $LOCUS_SIZE 2>/dev/null || echo "${LOCUS_SIZE} bytes"))${NC}"
else
    # Remove invalid/truncated file if it exists
    [ -f "$LOCUS_IDX" ] && rm -f "$LOCUS_IDX"
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

    # Index file created - wait for write to complete
    if [ -f "$LOCUS_IDX" ]; then
        echo ""
        echo -n "  Writing"
        PREV_SIZE=0
        STABLE_COUNT=0
        while [ $STABLE_COUNT -lt 3 ]; do
            if ! kill -0 $LOCUS_BUILD_PID 2>/dev/null; then
                # Process exited - file should be complete
                break
            fi
            CURR_SIZE=$(get_file_size "$LOCUS_IDX")
            if [ "$CURR_SIZE" = "$PREV_SIZE" ]; then
                STABLE_COUNT=$((STABLE_COUNT + 1))
            else
                STABLE_COUNT=0
                PREV_SIZE=$CURR_SIZE
            fi
            printf "."
            sleep 2
        done
        echo ""
        echo -e "  ${GREEN}Built: $LOCUS_IDX ($(numfmt --to=iec-i --suffix=B $CURR_SIZE 2>/dev/null || echo "${CURR_SIZE} bytes"))${NC}"
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

# Carta tile server (port 8081) - use index if available
# Use 8 worker threads for parallel tile generation
# LOD filtering enabled (OSM Carto-style zoom-dependent feature visibility)
# Demo mode: higher rate limits (50 RPS, 200 burst) for smoother local experience
# NOTE: Rate limit args must come before Carta-specific args like --lod
if [ -n "$CARTA_IDX" ] && [ -f "$CARTA_IDX" ]; then
    ./carta/api/carta-tile-server -p 8081 -t 8 \
        --rate-limit-rps 50 --rate-limit-burst 200 \
        --lod default "$CARTA_IDX" >/dev/null 2>&1 &
    CARTA_PID=$!
    echo "  Started: Carta (http://localhost:8081) [PID: $CARTA_PID] - index, 8 threads, 50 RPS"
else
    ./carta/api/carta-tile-server -p 8081 -t 8 \
        --rate-limit-rps 50 --rate-limit-burst 200 \
        --lod default "$PBF_FILE" >/dev/null 2>&1 &
    CARTA_PID=$!
    echo "  Started: Carta (http://localhost:8081) [PID: $CARTA_PID] - PBF, 8 threads, 50 RPS"
fi

# Velo route server (port 8082)
./velo/api/velo-route-server -p 8082 "$PBF_FILE" >/dev/null 2>&1 &
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
# Serve from project root so that relative imports work:
# - Demo at clayshards/clay-shards-demo/ imports ../clay-shards-webgl (works)
# - WebGL imports ../../shared/js/ for resilience utilities (works from root)
python3 -m http.server 8000 >/dev/null 2>&1 &
DEMO_PID=$!
echo "  Started: Demo (http://localhost:8000/clayshards/clay-shards-demo/) [PID: $DEMO_PID]"

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
echo "  Demo:     http://localhost:8000/clayshards/clay-shards-demo/"
echo "  Carta:    http://localhost:8081"
echo "  Velo:     http://localhost:8082"
echo "  Locus:    http://localhost:8083"
echo ""
echo "Run './scripts/stop-demo-servers.sh' to stop all servers."
