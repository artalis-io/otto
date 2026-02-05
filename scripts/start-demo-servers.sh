#!/bin/bash
# Start all demo servers for ClayShards map demo
#
# Default ports (will find alternatives if occupied):
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

# Find an available port starting from preferred port, avoiding already-claimed ports
# Usage: find_available_port PREFERRED_PORT [EXCLUDE_PORT1 EXCLUDE_PORT2 ...]
# Returns: Available port number (may be higher than preferred if occupied)
find_available_port() {
    local port=$1
    shift
    local exclude="$*"
    local max_attempts=20
    local attempt=0

    while [ $attempt -lt $max_attempts ]; do
        # Check if port is in use by system
        local in_use=0
        if lsof -i ":$port" >/dev/null 2>&1; then
            in_use=1
        fi

        # Check if port is in our exclude list
        if [ $in_use -eq 0 ]; then
            for ex in $exclude; do
                if [ "$port" = "$ex" ]; then
                    in_use=1
                    break
                fi
            done
        fi

        if [ $in_use -eq 0 ]; then
            echo $port
            return 0
        fi
        port=$((port + 1))
        attempt=$((attempt + 1))
    done

    # Failed to find available port
    echo ""
    return 1
}

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
VELO_IDX="$INDEX_DIR/$REGION-velo.vlg"

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
    # Build index and save it - runs synchronously with --build-only (exits after saving)
    echo -n "  "
    ./carta/api/carta-tile-server --build-only --no-lod -S "$CARTA_IDX" "$PBF_FILE" 2>&1 | while read line; do
        printf "."
    done
    echo ""
    if is_valid_index "$CARTA_IDX" 1048576; then
        CARTA_SIZE=$(get_file_size "$CARTA_IDX")
        echo -e "  ${GREEN}Built: $CARTA_IDX ($(numfmt --to=iec-i --suffix=B $CARTA_SIZE 2>/dev/null || echo "${CARTA_SIZE} bytes"))${NC}"
    else
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
    # Build index and save it - runs synchronously with --build-only (exits after saving)
    echo -n "  "
    ./locus/api/locus-geocoder --build-only -s "$LOCUS_IDX" "$PBF_FILE" 2>&1 | while read line; do
        printf "."
    done
    echo ""
    if is_valid_index "$LOCUS_IDX" 102400; then
        LOCUS_SIZE=$(get_file_size "$LOCUS_IDX")
        echo -e "  ${GREEN}Built: $LOCUS_IDX ($(numfmt --to=iec-i --suffix=B $LOCUS_SIZE 2>/dev/null || echo "${LOCUS_SIZE} bytes"))${NC}"
    else
        echo -e "  ${YELLOW}Index not created, using PBF directly${NC}"
    fi
fi

# Check for Velo index (minimum 1MB for valid graph)
# This block is independent of other index status
if is_valid_index "$VELO_IDX" 1048576; then
    VELO_SIZE=$(get_file_size "$VELO_IDX")
    echo -e "  ${GREEN}Found: $VELO_IDX ($(numfmt --to=iec-i --suffix=B $VELO_SIZE 2>/dev/null || echo "${VELO_SIZE} bytes"))${NC}"
else
    # Remove invalid/truncated file if it exists
    [ -f "$VELO_IDX" ] && rm -f "$VELO_IDX"
    echo -e "  ${YELLOW}Building Velo index (this may take a while)...${NC}"
    # Build index and save it - runs synchronously with --build-only (exits after saving)
    echo -n "  "
    ./velo/api/velo-route-server --build-only --no-landmarks --save-index "$VELO_IDX" "$PBF_FILE" 2>&1 | while read line; do
        printf "."
    done
    echo ""
    if is_valid_index "$VELO_IDX" 1048576; then
        VELO_SIZE=$(get_file_size "$VELO_IDX")
        echo -e "  ${GREEN}Built: $VELO_IDX ($(numfmt --to=iec-i --suffix=B $VELO_SIZE 2>/dev/null || echo "${VELO_SIZE} bytes"))${NC}"
    else
        echo -e "  ${YELLOW}Velo index not created, using PBF directly${NC}"
        VELO_IDX=""
    fi
fi
echo ""

# Find available ports (will try alternatives if preferred ports are occupied)
# Each call passes already-claimed ports to avoid collisions
echo "Finding available ports..."
CARTA_PORT=$(find_available_port 8081)
VELO_PORT=$(find_available_port 8082 $CARTA_PORT)
LOCUS_PORT=$(find_available_port 8083 $CARTA_PORT $VELO_PORT)
DEMO_PORT=$(find_available_port 8000 $CARTA_PORT $VELO_PORT $LOCUS_PORT)

if [ -z "$CARTA_PORT" ] || [ -z "$VELO_PORT" ] || [ -z "$LOCUS_PORT" ] || [ -z "$DEMO_PORT" ]; then
    echo -e "${RED}Error: Could not find available ports${NC}"
    exit 1
fi

if [ "$CARTA_PORT" != "8081" ] || [ "$VELO_PORT" != "8082" ] || [ "$LOCUS_PORT" != "8083" ]; then
    echo -e "  ${YELLOW}Note: Using non-default ports (some were occupied)${NC}"
fi
echo ""

# Start servers in background
echo "Starting servers..."

# Carta tile server - use index if available
# 8 worker threads, no LOD filtering (for debugging), higher rate limits for demo
if [ -n "$CARTA_IDX" ] && [ -f "$CARTA_IDX" ]; then
    ./carta/api/carta-tile-server -p "$CARTA_PORT" -t 8 \
        --rate-limit-rps 50 --rate-limit-burst 200 \
        --no-lod "$CARTA_IDX" >/dev/null 2>&1 &
    CARTA_PID=$!
    echo "  Started: Carta (http://localhost:$CARTA_PORT) [PID: $CARTA_PID] - index"
else
    ./carta/api/carta-tile-server -p "$CARTA_PORT" -t 8 \
        --rate-limit-rps 50 --rate-limit-burst 200 \
        --no-lod "$PBF_FILE" >/dev/null 2>&1 &
    CARTA_PID=$!
    echo "  Started: Carta (http://localhost:$CARTA_PORT) [PID: $CARTA_PID] - PBF"
fi

# Velo route server - use index if available
# Using --no-landmarks for faster startup (routing still works, just slightly slower)
if [ -n "$VELO_IDX" ] && [ -f "$VELO_IDX" ]; then
    ./velo/api/velo-route-server -p "$VELO_PORT" --no-landmarks "$VELO_IDX" >/dev/null 2>&1 &
    VELO_PID=$!
    echo "  Started: Velo (http://localhost:$VELO_PORT) [PID: $VELO_PID] - index"
else
    ./velo/api/velo-route-server -p "$VELO_PORT" --no-landmarks "$PBF_FILE" >/dev/null 2>&1 &
    VELO_PID=$!
    echo "  Started: Velo (http://localhost:$VELO_PORT) [PID: $VELO_PID] - PBF"
fi

# Locus geocoding server - use index if available
if [ -f "$LOCUS_IDX" ]; then
    ./locus/api/locus-geocoder -p "$LOCUS_PORT" "$LOCUS_IDX" >/dev/null 2>&1 &
    LOCUS_PID=$!
    echo "  Started: Locus (http://localhost:$LOCUS_PORT) [PID: $LOCUS_PID] - index"
else
    ./locus/api/locus-geocoder -p "$LOCUS_PORT" "$PBF_FILE" >/dev/null 2>&1 &
    LOCUS_PID=$!
    echo "  Started: Locus (http://localhost:$LOCUS_PORT) [PID: $LOCUS_PID] - PBF"
fi

# Generate demo config file with actual server URLs
CONFIG_FILE="demo-config.json"
cat > "$CONFIG_FILE" << EOF
{
  "carta": "http://localhost:$CARTA_PORT",
  "velo": "http://localhost:$VELO_PORT",
  "locus": "http://localhost:$LOCUS_PORT"
}
EOF
echo "  Generated: $CONFIG_FILE"

# Start demo HTTP server - serves from project root
python3 -m http.server "$DEMO_PORT" >/dev/null 2>&1 &
DEMO_PID=$!
echo "  Started: Demo (http://localhost:$DEMO_PORT/clayshards/clay-shards-demo/) [PID: $DEMO_PID]"

echo ""
echo "Waiting for servers to be ready..."

# Wait for server health endpoint
wait_for_health() {
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

# Wait for Velo to actually have graph loaded (not just health OK)
wait_for_velo_ready() {
    local port=$1
    local timeout=$2

    echo -n "  Velo: waiting for graph..."
    for i in $(seq 1 $timeout); do
        # Check if stats endpoint shows nodes > 0 (graph is loaded)
        local nodes=$(curl -s "http://localhost:$port/api/v1/stats" 2>/dev/null | grep -o '"num_nodes": *[0-9]*' | grep -o '[0-9]*' || echo "0")
        if [ "$nodes" -gt 0 ] 2>/dev/null; then
            echo -e "\r  ${GREEN}Velo ready${NC} ($nodes nodes loaded)        "
            return 0
        fi
        printf "."
        sleep 1
    done
    echo -e "\r  ${YELLOW}Velo timeout (may still be loading)${NC}        "
    return 1
}

# Wait for Locus to actually have index loaded
wait_for_locus_ready() {
    local port=$1
    local timeout=$2

    echo -n "  Locus: waiting for index..."
    for i in $(seq 1 $timeout); do
        # Check if stats endpoint shows entities > 0 (index is loaded)
        local entities=$(curl -s "http://localhost:$port/api/v1/stats" 2>/dev/null | grep -o '"entities": *[0-9]*' | grep -o '[0-9]*' || echo "0")
        if [ "$entities" -gt 0 ] 2>/dev/null; then
            echo -e "\r  ${GREEN}Locus ready${NC} ($entities entities loaded)        "
            return 0
        fi
        printf "."
        sleep 1
    done
    echo -e "\r  ${YELLOW}Locus timeout (may still be loading)${NC}        "
    return 1
}

# Wait for servers - Carta just needs health, Velo/Locus need data loaded
wait_for_health "Carta" "http://localhost:$CARTA_PORT/api/v1/health" 30
wait_for_velo_ready "$VELO_PORT" 120
wait_for_locus_ready "$LOCUS_PORT" 300

echo ""
echo -e "${GREEN}=== All servers running ===${NC}"
echo ""
echo "  Demo:     http://localhost:$DEMO_PORT/clayshards/clay-shards-demo/"
echo "  Carta:    http://localhost:$CARTA_PORT"
echo "  Velo:     http://localhost:$VELO_PORT"
echo "  Locus:    http://localhost:$LOCUS_PORT"
echo ""
echo "Run './scripts/stop-demo-servers.sh' to stop all servers."
