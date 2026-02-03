#!/bin/bash
#
# Threading stress test for Carta tile server
#
# Tests concurrent request handling to verify thread safety:
# - Parallel PNG tile requests
# - Parallel MVT tile requests
# - Mixed PNG/MVT requests
# - Same-tile contention (cache races)
# - Different-tile parallelism
#
# Usage:
#   ./test_threading.sh                    # Run with defaults
#   ./test_threading.sh --threads 8        # Server threads
#   ./test_threading.sh --clients 16       # Concurrent clients
#   ./test_threading.sh --requests 500     # Total requests per test
#   ./test_threading.sh --pbf path/to.pbf  # Custom PBF file
#

set -e

# Default configuration
SERVER_THREADS="${CARTA_THREADS:-4}"
CONCURRENT_CLIENTS=16
TOTAL_REQUESTS=200
PORT=18091
PBF_FILE="${PBF_FILE:-../../data/hungary-latest.osm.pbf}"
TIMEOUT_SEC=30
VERBOSE=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[PASS]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[FAIL]${NC} $1"; }
header()  { echo -e "\n${CYAN}=== $1 ===${NC}\n"; }

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --threads)   SERVER_THREADS="$2"; shift 2 ;;
        --clients)   CONCURRENT_CLIENTS="$2"; shift 2 ;;
        --requests)  TOTAL_REQUESTS="$2"; shift 2 ;;
        --port)      PORT="$2"; shift 2 ;;
        --pbf)       PBF_FILE="$2"; shift 2 ;;
        --timeout)   TIMEOUT_SEC="$2"; shift 2 ;;
        --verbose)   VERBOSE=1; shift ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --threads N    Server worker threads (default: $SERVER_THREADS)"
            echo "  --clients N    Concurrent client requests (default: $CONCURRENT_CLIENTS)"
            echo "  --requests N   Total requests per test (default: $TOTAL_REQUESTS)"
            echo "  --port N       Server port (default: $PORT)"
            echo "  --pbf FILE     PBF file path (default: $PBF_FILE)"
            echo "  --timeout N    Request timeout in seconds (default: $TIMEOUT_SEC)"
            echo "  --verbose      Show individual request details"
            exit 0
            ;;
        *) error "Unknown option: $1"; exit 1 ;;
    esac
done

# Validate PBF file
if [[ ! -f "$PBF_FILE" ]]; then
    error "PBF file not found: $PBF_FILE"
    info "Run: ./scripts/download-osm.sh hungary"
    exit 1
fi

# Build server if needed
if [[ ! -f "./carta-tile-server" ]]; then
    info "Building carta-tile-server..."
    make -C . all || { error "Build failed"; exit 1; }
fi

# Cleanup function
cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$RESULTS_FILE" "$ERROR_FILE" "$SERVER_LOG" 2>/dev/null || true
}
trap cleanup EXIT

# Temporary files for results
RESULTS_FILE=$(mktemp)
ERROR_FILE=$(mktemp)

# Start server
header "Starting Carta Tile Server"
info "PBF file: $PBF_FILE"
info "Server threads: $SERVER_THREADS"
info "Concurrent clients: $CONCURRENT_CLIENTS"
info "Requests per test: $TOTAL_REQUESTS"

SERVER_LOG=$(mktemp)
./carta-tile-server --threads "$SERVER_THREADS" -p "$PORT" "$PBF_FILE" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

# Wait for server to be ready
info "Waiting for server to load PBF..."
WAIT_COUNT=0
MAX_WAIT=60
while ! curl -s "http://localhost:$PORT/api/v1/health" > /dev/null 2>&1; do
    sleep 1
    WAIT_COUNT=$((WAIT_COUNT + 1))
    if [[ $WAIT_COUNT -ge $MAX_WAIT ]]; then
        error "Server failed to start within ${MAX_WAIT}s"
        if [[ -f "$SERVER_LOG" ]]; then
            echo "Server log:"
            tail -20 "$SERVER_LOG"
        fi
        exit 1
    fi
    # Check if server process died
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        error "Server process died during startup"
        if [[ -f "$SERVER_LOG" ]]; then
            echo "Server log:"
            tail -20 "$SERVER_LOG"
        fi
        exit 1
    fi
done
success "Server ready (loaded in ${WAIT_COUNT}s)"

# Check for worker thread warnings in server log
if grep -q "Cannot listen" "$SERVER_LOG" 2>/dev/null; then
    warn "Some worker threads failed to bind (SO_REUSEPORT may not be available)"
    warn "Server running with reduced thread count"
fi

# Verify server is using multiple threads
STATS=$(curl -s "http://localhost:$PORT/api/v1/stats")
if echo "$STATS" | grep -q '"threads"'; then
    ACTUAL_THREADS=$(echo "$STATS" | grep -o '"threads":[0-9]*' | cut -d: -f2)
    info "Server reports $ACTUAL_THREADS worker threads"
fi

# Generate tile coordinates for Hungary (centered around Budapest)
# At z=10: Hungary spans roughly x=560-580, y=350-365
# Tile coordinates must be valid: 0 <= x,y < 2^z
generate_tile_urls() {
    local format=$1  # png or mvt
    local count=$2
    local mode=$3    # random, same, sequential

    for i in $(seq 1 "$count"); do
        case $mode in
            same)
                # All requests for same tile (cache contention test)
                echo "http://localhost:$PORT/tiles/10/567/357.$format"
                ;;
            sequential)
                # Sequential tiles at z=10 (minimal cache benefit)
                local x=$((567 + (i % 10)))
                local y=$((357 + (i / 10)))
                echo "http://localhost:$PORT/tiles/10/$x/$y.$format"
                ;;
            random)
                # Random tiles across Hungary at fixed zoom (z=10)
                # At z=10, valid range is 0-1023, Hungary is ~560-580, 350-365
                local x=$((560 + RANDOM % 20))
                local y=$((350 + RANDOM % 15))
                echo "http://localhost:$PORT/tiles/10/$x/$y.$format"
                ;;
        esac
    done
}

# Run concurrent request test
run_test() {
    local name=$1
    local format=$2
    local mode=$3

    header "Test: $name"

    # Check server is still running
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        error "Server is not running - skipping test"
        if [[ -f "$SERVER_LOG" ]]; then
            echo "Last lines of server log:"
            tail -10 "$SERVER_LOG"
        fi
        return 1
    fi

    # Generate URLs
    local urls_file=$(mktemp)
    generate_tile_urls "$format" "$TOTAL_REQUESTS" "$mode" > "$urls_file"

    # Clear results
    > "$RESULTS_FILE"
    > "$ERROR_FILE"

    # Run concurrent requests with timing
    local start_time=$(date +%s%N)

    cat "$urls_file" | xargs -P "$CONCURRENT_CLIENTS" -I {} \
        curl -s -o /dev/null -w "%{http_code} %{time_total}\n" \
        --max-time "$TIMEOUT_SEC" {} 2>>"$ERROR_FILE" >> "$RESULTS_FILE"

    local end_time=$(date +%s%N)
    local duration_ms=$(( (end_time - start_time) / 1000000 ))

    # Analyze results
    local total
    total=$(wc -l < "$RESULTS_FILE")
    total=${total// /}
    local success_count
    success_count=$(grep -c "^200 " "$RESULTS_FILE" 2>/dev/null) || success_count=0
    local error_count=$((total - success_count))
    local timeout_count
    timeout_count=$(grep -c "^000 " "$RESULTS_FILE" 2>/dev/null) || timeout_count=0

    # Calculate timing stats
    local times=$(awk '{print $2}' "$RESULTS_FILE" | sort -n)
    local min_time=$(echo "$times" | head -1)
    local max_time=$(echo "$times" | tail -1)
    local avg_time=$(echo "$times" | awk '{sum+=$1} END {printf "%.3f", sum/NR}')
    local p50_time=$(echo "$times" | awk -v n="$total" 'NR==int(n*0.5){print}')
    local p95_time=$(echo "$times" | awk -v n="$total" 'NR==int(n*0.95){print}')
    local p99_time=$(echo "$times" | awk -v n="$total" 'NR==int(n*0.99){print}')

    # Calculate throughput
    local throughput=$(awk "BEGIN {printf \"%.1f\", $total / ($duration_ms / 1000)}")

    # Report results
    echo "  Requests:    $total total, $success_count success, $error_count errors, $timeout_count timeouts"
    echo "  Duration:    ${duration_ms}ms total"
    echo "  Throughput:  ${throughput} req/s"
    echo "  Latency:     min=${min_time}s avg=${avg_time}s max=${max_time}s"
    echo "  Percentiles: p50=${p50_time}s p95=${p95_time}s p99=${p99_time}s"

    # Check for failures
    if [[ $error_count -gt 0 ]]; then
        warn "  Non-200 responses detected:"
        grep -v "^200 " "$RESULTS_FILE" | head -5 | while read code time; do
            echo "    HTTP $code (${time}s)"
        done
    fi

    # Check for curl errors
    if [[ -s "$ERROR_FILE" ]]; then
        warn "  Curl errors detected:"
        head -3 "$ERROR_FILE"
    fi

    rm -f "$urls_file"

    # Return success/failure
    if [[ $success_count -eq $total ]]; then
        success "All $total requests successful"
        return 0
    else
        error "$error_count/$total requests failed"
        return 1
    fi
}

# Run mixed format test
run_mixed_test() {
    header "Test: Mixed PNG/MVT Concurrent Requests"

    local urls_file=$(mktemp)

    # Generate alternating PNG and MVT requests
    for i in $(seq 1 "$TOTAL_REQUESTS"); do
        local x=$((550 + RANDOM % 40))
        local y=$((345 + RANDOM % 30))
        if (( i % 2 == 0 )); then
            echo "http://localhost:$PORT/tiles/10/$x/$y.png"
        else
            echo "http://localhost:$PORT/tiles/10/$x/$y.mvt"
        fi
    done > "$urls_file"

    > "$RESULTS_FILE"
    > "$ERROR_FILE"

    local start_time=$(date +%s%N)

    cat "$urls_file" | xargs -P "$CONCURRENT_CLIENTS" -I {} \
        curl -s -o /dev/null -w "%{http_code} %{time_total}\n" \
        --max-time "$TIMEOUT_SEC" {} 2>>"$ERROR_FILE" >> "$RESULTS_FILE"

    local end_time=$(date +%s%N)
    local duration_ms=$(( (end_time - start_time) / 1000000 ))

    local total
    total=$(wc -l < "$RESULTS_FILE")
    total=${total// /}
    local success_count
    success_count=$(grep -c "^200 " "$RESULTS_FILE" 2>/dev/null) || success_count=0
    local error_count=$((total - success_count))
    local throughput
    throughput=$(awk "BEGIN {printf \"%.1f\", $total / ($duration_ms / 1000)}")

    echo "  Requests:    $total total, $success_count success, $error_count errors"
    echo "  Duration:    ${duration_ms}ms"
    echo "  Throughput:  ${throughput} req/s"

    rm -f "$urls_file"

    if [[ $success_count -eq $total ]]; then
        success "All $total mixed requests successful"
        return 0
    else
        error "$error_count/$total requests failed"
        return 1
    fi
}

# Run cache race condition test
run_cache_race_test() {
    header "Test: Cache Race Condition (Same Tile)"

    info "Testing concurrent requests for identical tile..."
    info "This tests mutex protection of cache get/put"

    # All requests for the exact same tile
    local urls_file=$(mktemp)
    for i in $(seq 1 100); do
        echo "http://localhost:$PORT/tiles/10/567/357.png"
    done > "$urls_file"

    > "$RESULTS_FILE"

    # High concurrency on same resource
    cat "$urls_file" | xargs -P 32 -I {} \
        curl -s -o /dev/null -w "%{http_code}\n" \
        --max-time "$TIMEOUT_SEC" {} >> "$RESULTS_FILE"

    local total
    total=$(wc -l < "$RESULTS_FILE")
    total=${total// /}
    local success_count
    success_count=$(grep -c "^200$" "$RESULTS_FILE" 2>/dev/null) || success_count=0

    rm -f "$urls_file"

    if [[ $success_count -eq $total ]]; then
        success "Cache race test passed ($total concurrent same-tile requests)"
        return 0
    else
        error "Cache race test failed: $((total - success_count))/$total errors"
        return 1
    fi
}

# Run server stability test
run_stability_test() {
    header "Test: Server Stability Under Load"

    info "Running extended load test..."

    local start_pid_count=$(ps aux | grep -c "[c]arta-tile-server" || echo 0)

    # Run 3 waves of requests
    for wave in 1 2 3; do
        echo "  Wave $wave/3..."
        generate_tile_urls "png" 100 "random" | \
            xargs -P "$CONCURRENT_CLIENTS" -I {} \
            curl -s -o /dev/null --max-time "$TIMEOUT_SEC" {}
    done

    # Check server is still running
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        success "Server remained stable through load test"

        # Check health endpoint still works
        if curl -s "http://localhost:$PORT/api/v1/health" | grep -q '"status"'; then
            success "Health endpoint responsive after load"
        else
            warn "Health endpoint degraded after load"
        fi
        return 0
    else
        error "Server crashed during load test!"
        return 1
    fi
}

# Main test suite
header "Carta Threading Stress Test"
echo "Configuration:"
echo "  Server threads:     $SERVER_THREADS"
echo "  Concurrent clients: $CONCURRENT_CLIENTS"
echo "  Requests per test:  $TOTAL_REQUESTS"
echo "  Timeout:            ${TIMEOUT_SEC}s"

TESTS_PASSED=0
TESTS_FAILED=0

# Run test suite
if run_test "PNG Sequential Tiles" "png" "sequential"; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

if run_test "PNG Random Tiles" "png" "random"; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

if run_test "MVT Sequential Tiles" "mvt" "sequential"; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

if run_test "MVT Random Tiles" "mvt" "random"; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

if run_mixed_test; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

if run_cache_race_test; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

if run_stability_test; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
else
    TESTS_FAILED=$((TESTS_FAILED + 1))
fi

# Summary
header "Test Summary"
echo "  Passed: $TESTS_PASSED"
echo "  Failed: $TESTS_FAILED"
echo ""

if [[ $TESTS_FAILED -eq 0 ]]; then
    success "All threading tests passed!"
    exit 0
else
    error "$TESTS_FAILED test(s) failed"
    exit 1
fi
