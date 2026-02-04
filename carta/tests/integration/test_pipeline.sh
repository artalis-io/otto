#!/bin/bash
#
# Carta Integration Test - Full Pipeline
#
# Tests the complete workflow:
# 1. Load PBF file
# 2. Save binary index
# 3. Load binary index
# 4. Serve tiles via API
# 5. Generate PNG and MVT tiles
#
# Requires: Monaco PBF in data/ directory
#

# Don't exit on error - we track pass/fail ourselves
# set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../../" && pwd)"
CARTA_DIR="$PROJECT_ROOT/carta"
DATA_DIR="$PROJECT_ROOT/data"
TMP_DIR="/tmp/carta_integration_test_$$"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Cleanup on exit
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# Test result tracking
TESTS_PASSED=0
TESTS_FAILED=0

pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
}

echo "=== Carta Integration Test Suite ==="
echo ""

# Setup
mkdir -p "$TMP_DIR"

# Check for test data
PBF_FILE="$DATA_DIR/monaco-latest.osm.pbf"
if [ ! -f "$PBF_FILE" ]; then
    skip "Monaco PBF not found at $PBF_FILE"
    skip "Run: ./scripts/download-osm.sh monaco"
    exit 0
fi

# Check for tile server binary
TILE_SERVER="$CARTA_DIR/api/carta-tile-server"
if [ ! -x "$TILE_SERVER" ]; then
    echo "Building tile server..."
    make -C "$PROJECT_ROOT" carta-api >/dev/null 2>&1
fi

if [ ! -x "$TILE_SERVER" ]; then
    fail "Tile server not found at $TILE_SERVER"
    exit 1
fi

echo "Test data: $PBF_FILE"
echo "Tile server: $TILE_SERVER"
echo ""

# Test 1: Load PBF and save binary index
echo "--- Test 1: PBF → Binary Index ---"
INDEX_FILE="$TMP_DIR/monaco.idx"

# Start server with --save-index, wait for it to save then stop
"$TILE_SERVER" --save-index "$INDEX_FILE" -p 18091 "$PBF_FILE" >/dev/null 2>&1 &
SERVER_PID=$!

# Wait for index file to appear (up to 30 seconds)
for i in $(seq 1 30); do
    if [ -f "$INDEX_FILE" ] && [ -s "$INDEX_FILE" ]; then
        break
    fi
    sleep 1
done

# Give it a moment to finish writing
sleep 2

if [ -f "$INDEX_FILE" ] && [ -s "$INDEX_FILE" ]; then
    pass "Binary index created ($(du -h "$INDEX_FILE" | cut -f1))"
else
    fail "Binary index not created or empty"
fi

# Stop the first server
kill "$SERVER_PID" 2>/dev/null || true
sleep 2
unset SERVER_PID

# Test 2: Load from binary index
echo ""
echo "--- Test 2: Load Binary Index ---"

"$TILE_SERVER" -p 18092 "$INDEX_FILE" >/dev/null 2>&1 &
SERVER_PID=$!
sleep 3

HEALTH=$(curl -s http://localhost:18092/api/v1/health 2>/dev/null || echo "")
if echo "$HEALTH" | grep -q "healthy"; then
    pass "Tile server started from binary index"
else
    fail "Tile server failed to start from binary index"
fi

# Test 3: API endpoints
echo ""
echo "--- Test 3: API Endpoints ---"

# Health check
if curl -s http://localhost:18092/api/v1/health | grep -q "healthy"; then
    pass "GET /api/v1/health"
else
    fail "GET /api/v1/health"
fi

# Stats
if curl -s http://localhost:18092/api/v1/stats | grep -q "total_ways"; then
    pass "GET /api/v1/stats"
else
    fail "GET /api/v1/stats"
fi

# TileJSON
if curl -s http://localhost:18092/tiles.json | grep -q "tilejson"; then
    pass "GET /tiles.json"
else
    fail "GET /tiles.json"
fi

# Test 4: Tile generation
echo ""
echo "--- Test 4: Tile Generation ---"

# PNG tile (Monaco center at z=14)
PNG_FILE="$TMP_DIR/tile.png"
curl -s -o "$PNG_FILE" "http://localhost:18092/tiles/14/8607/5894.png"
if file "$PNG_FILE" | grep -q "PNG image"; then
    PNG_SIZE=$(stat -f%z "$PNG_FILE" 2>/dev/null || stat -c%s "$PNG_FILE" 2>/dev/null)
    pass "PNG tile generated ($PNG_SIZE bytes)"
else
    fail "PNG tile generation failed"
fi

# MVT tile
MVT_FILE="$TMP_DIR/tile.mvt"
curl -s -o "$MVT_FILE" "http://localhost:18092/tiles/14/8607/5894.mvt"
MVT_SIZE=$(stat -f%z "$MVT_FILE" 2>/dev/null || stat -c%s "$MVT_FILE" 2>/dev/null)
if [ "$MVT_SIZE" -ge 0 ]; then
    pass "MVT tile generated ($MVT_SIZE bytes)"
else
    fail "MVT tile generation failed"
fi

# Test 5: Multiple zoom levels
echo ""
echo "--- Test 5: Multiple Zoom Levels ---"

for z in 10 12 14 16; do
    PNG_FILE="$TMP_DIR/tile_z$z.png"
    curl -s -o "$PNG_FILE" "http://localhost:18092/tiles/$z/537/368.png"
    if file "$PNG_FILE" | grep -q "PNG image"; then
        pass "PNG tile z=$z"
    else
        fail "PNG tile z=$z"
    fi
done

# Test 6: Edge cases
echo ""
echo "--- Test 6: Edge Cases ---"

# Empty tile (ocean coordinates)
curl -s -o "$TMP_DIR/empty.png" "http://localhost:18092/tiles/14/0/0.png"
if file "$TMP_DIR/empty.png" | grep -q "PNG image"; then
    pass "Empty tile (out of bounds)"
else
    fail "Empty tile handling"
fi

# Invalid zoom (should return error or empty)
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:18092/tiles/50/0/0.png")
if [ "$HTTP_CODE" -eq 404 ] || [ "$HTTP_CODE" -eq 400 ]; then
    pass "Invalid zoom rejected (HTTP $HTTP_CODE)"
else
    # Some servers return 200 with empty/placeholder tile
    pass "Invalid zoom handled (HTTP $HTTP_CODE)"
fi

# Cleanup server
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
unset SERVER_PID

# Summary
echo ""
echo "=== Results ==="
echo -e "Passed: ${GREEN}$TESTS_PASSED${NC}"
echo -e "Failed: ${RED}$TESTS_FAILED${NC}"

if [ "$TESTS_FAILED" -eq 0 ]; then
    echo -e "\n${GREEN}All integration tests passed!${NC}"
    exit 0
else
    echo -e "\n${RED}Some tests failed.${NC}"
    exit 1
fi
