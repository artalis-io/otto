#\!/bin/bash
#
# Run performance benchmarks for FuelWise Platform
#
# Usage:
#   ./scripts/benchmark.sh [component]
#   ./scripts/benchmark.sh           # Run all benchmarks
#   ./scripts/benchmark.sh ralph     # Ralph solver only
#   ./scripts/benchmark.sh velo      # Velo routing only
#   ./scripts/benchmark.sh carta     # Carta tiles only
#   ./scripts/benchmark.sh api       # API latency tests
#

set -e

# Configuration
DATA_DIR="${DATA_DIR:-./data}"
PBF_FILE="${PBF_FILE:-$DATA_DIR/hungary-latest.osm.pbf}"
RESULTS_DIR="${RESULTS_DIR:-./benchmark-results}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info() { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[PASS]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[FAIL]${NC} $1"; }

# Create results directory
mkdir -p "$RESULTS_DIR"

# Benchmark Ralph solver
bench_ralph() {
    info "Benchmarking Ralph LP/MIP solver..."
    
    if [[ \! -f ralph/benchmarks/bench_ralph ]]; then
        info "Building Ralph benchmarks..."
        make -C ralph bench 2>/dev/null || {
            warn "Ralph benchmarks not available, skipping"
            return
        }
    fi
    
    local output="$RESULTS_DIR/ralph_$TIMESTAMP.txt"
    echo "Ralph Benchmark Results - $(date)" > "$output"
    echo "=================================" >> "$output"
    
    ./ralph/benchmarks/bench_ralph 2>&1 | tee -a "$output"
    
    success "Ralph benchmark complete: $output"
}

# Benchmark Velo routing
bench_velo() {
    info "Benchmarking Velo routing engine..."
    
    if [[ \! -f "$PBF_FILE" ]]; then
        error "PBF file not found: $PBF_FILE"
        warn "Run: ./scripts/download-osm.sh hungary"
        return
    fi
    
    if [[ \! -f velo/benchmarks/bench_velo ]]; then
        info "Building Velo benchmarks..."
        make -C velo bench 2>/dev/null || {
            warn "Velo benchmarks not available, building..."
            make velo
        }
    fi
    
    local output="$RESULTS_DIR/velo_$TIMESTAMP.txt"
    echo "Velo Routing Benchmark Results - $(date)" > "$output"
    echo "=========================================" >> "$output"
    echo "PBF: $PBF_FILE" >> "$output"
    echo "" >> "$output"
    
    # If bench_velo exists, run it
    if [[ -f velo/benchmarks/bench_velo ]]; then
        ./velo/benchmarks/bench_velo "$PBF_FILE" 2>&1 | tee -a "$output"
    else
        # Manual benchmark using route server
        info "Running manual routing benchmark..."
        
        # Start server temporarily
        ./velo/api/velo-route-server -p 18082 "$PBF_FILE" &
        SERVER_PID=$\!
        sleep 15  # Wait for landmarks
        
        echo "Benchmark: 100 random routes" >> "$output"
        echo "" >> "$output"
        
        # Time 100 route requests
        local start=$(date +%s%N)
        for i in $(seq 1 100); do
            curl -s "http://localhost:18082/api/v1/route?from=47.497,19.040&to=46.253,20.148" > /dev/null
        done
        local end=$(date +%s%N)
        local duration=$(( (end - start) / 1000000 ))
        local avg=$(( duration / 100 ))
        
        echo "Total time: ${duration}ms" >> "$output"
        echo "Average: ${avg}ms/route" >> "$output"
        
        kill $SERVER_PID 2>/dev/null || true
    fi
    
    success "Velo benchmark complete: $output"
}

# Benchmark Carta tile generation
bench_carta() {
    info "Benchmarking Carta tile generator..."
    
    if [[ \! -f "$PBF_FILE" ]]; then
        error "PBF file not found: $PBF_FILE"
        warn "Run: ./scripts/download-osm.sh hungary"
        return
    fi
    
    local output="$RESULTS_DIR/carta_$TIMESTAMP.txt"
    echo "Carta Tile Benchmark Results - $(date)" > "$output"
    echo "======================================" >> "$output"
    echo "PBF: $PBF_FILE" >> "$output"
    echo "" >> "$output"
    
    # Start server temporarily
    info "Starting tile server..."
    ./carta/api/carta-tile-server -p 18081 "$PBF_FILE" &
    SERVER_PID=$\!
    sleep 10  # Wait for PBF load
    
    # Test tile generation at different zoom levels
    echo "PNG Tile Generation Times:" >> "$output"
    for zoom in 8 10 12 14; do
        local start=$(date +%s%N)
        for i in $(seq 1 10); do
            # Tile coords for Hungary center at different zooms
            local n=$((1 << zoom))
            local x=$((n * 37 / 100))  # ~19.0 lon
            local y=$((n * 28 / 100))  # ~47.5 lat
            curl -s "http://localhost:18081/tiles/$zoom/$x/$y.png" > /dev/null
        done
        local end=$(date +%s%N)
        local avg=$(( (end - start) / 10000000 ))
        echo "  Zoom $zoom: ${avg}ms/tile" >> "$output"
    done
    
    echo "" >> "$output"
    echo "MVT Tile Generation Times:" >> "$output"
    for zoom in 8 10 12 14; do
        local start=$(date +%s%N)
        for i in $(seq 1 10); do
            local n=$((1 << zoom))
            local x=$((n * 37 / 100))
            local y=$((n * 28 / 100))
            curl -s "http://localhost:18081/tiles/$zoom/$x/$y.mvt" > /dev/null
        done
        local end=$(date +%s%N)
        local avg=$(( (end - start) / 10000000 ))
        echo "  Zoom $zoom: ${avg}ms/tile" >> "$output"
    done
    
    kill $SERVER_PID 2>/dev/null || true
    
    success "Carta benchmark complete: $output"
}

# Benchmark API latency
bench_api() {
    info "Benchmarking API latency..."
    
    local output="$RESULTS_DIR/api_$TIMESTAMP.txt"
    echo "API Latency Benchmark Results - $(date)" > "$output"
    echo "=======================================" >> "$output"
    echo "" >> "$output"
    
    # FuelWise API
    if pgrep -f "fuelwise-api" > /dev/null || [[ -f fuelwise/api/fuelwise-api ]]; then
        info "Testing FuelWise API..."
        
        ./fuelwise/api/fuelwise-api -p 18080 &
        API_PID=$\!
        sleep 2
        
        echo "FuelWise API (localhost:18080):" >> "$output"
        
        # Health endpoint
        local start=$(date +%s%N)
        for i in $(seq 1 100); do
            curl -s "http://localhost:18080/api/v1/health" > /dev/null
        done
        local end=$(date +%s%N)
        echo "  /health: $(( (end - start) / 100000000 ))ms avg" >> "$output"
        
        # Solve endpoint
        start=$(date +%s%N)
        for i in $(seq 1 50); do
            curl -s -X POST "http://localhost:18080/api/v1/solve" \
                -H "Content-Type: application/json" \
                -d '{"total_distance":500,"tank_capacity":100,"current_fuel":20,"consumption_mpg":10,"minimum_fuel":10,"stations":[{"id":1,"distance":100,"price":3.50},{"id":2,"distance":250,"price":3.25},{"id":3,"distance":400,"price":3.75}]}' > /dev/null
        done
        end=$(date +%s%N)
        echo "  /solve: $(( (end - start) / 50000000 ))ms avg" >> "$output"
        
        kill $API_PID 2>/dev/null || true
    fi
    
    echo "" >> "$output"
    success "API benchmark complete: $output"
}

# Summary
show_summary() {
    echo ""
    echo "================================="
    echo "Benchmark Results Summary"
    echo "================================="
    echo "Results saved to: $RESULTS_DIR"
    echo ""
    ls -la "$RESULTS_DIR"/*_$TIMESTAMP.txt 2>/dev/null || echo "No results generated"
}

# Main
main() {
    echo "FuelWise Platform Benchmark Suite"
    echo "================================="
    echo ""
    
    case "${1:-all}" in
        ralph)
            bench_ralph
            ;;
        velo)
            bench_velo
            ;;
        carta)
            bench_carta
            ;;
        api)
            bench_api
            ;;
        all)
            bench_ralph
            bench_velo
            bench_carta
            bench_api
            ;;
        *)
            echo "Usage: $0 [ralph|velo|carta|api|all]"
            exit 1
            ;;
    esac
    
    show_summary
}

main "$@"
