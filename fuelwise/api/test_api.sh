#!/bin/bash
#
# FuelWise API Test Suite
#
# Tests health, stats, solve, filter, and optimize endpoints
# Also tests rate limiting and work queue functionality
#

set -e

PORT=8099
PASS=0
FAIL=0
TESTS_RUN=0

# Parse arguments
QUICK=0
RATELIMIT_ONLY=0
QUEUE_ONLY=0
for arg in "$@"; do
    case $arg in
        --quick) QUICK=1 ;;
        --ratelimit) RATELIMIT_ONLY=1 ;;
        --queue) QUEUE_ONLY=1 ;;
    esac
done

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass() {
    PASS=$((PASS + 1))
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -e "  ${GREEN}PASS${NC}: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -e "  ${RED}FAIL${NC}: $1"
}

# Start server
start_server() {
    local extra_args="$1"
    ./fuelwise-api -p $PORT $extra_args > /dev/null 2>&1 &
    SERVER_PID=$!
    sleep 1

    # Verify server started
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "ERROR: Server failed to start"
        exit 1
    fi
}

# Stop server
stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
    fi
}

# Cleanup on exit
trap stop_server EXIT

# ============================================================================
# Basic Endpoint Tests
# ============================================================================

test_health() {
    echo ""
    echo "=== Testing Health Endpoint ==="

    RESP=$(curl -s http://localhost:$PORT/api/v1/health)

    if echo "$RESP" | grep -q '"status": "healthy"'; then
        pass "Health endpoint returns healthy status"
    else
        fail "Health endpoint did not return healthy status"
    fi

    if echo "$RESP" | grep -q '"service": "fuelwise-api"'; then
        pass "Health endpoint returns correct service name"
    else
        fail "Health endpoint missing service name"
    fi
}

test_stats() {
    echo ""
    echo "=== Testing Stats Endpoint ==="

    RESP=$(curl -s http://localhost:$PORT/api/v1/stats)

    if echo "$RESP" | grep -q '"work_queue"'; then
        pass "Stats endpoint returns work_queue info"
    else
        fail "Stats endpoint missing work_queue info"
    fi

    if echo "$RESP" | grep -q '"rate_limit"'; then
        pass "Stats endpoint returns rate_limit info"
    else
        fail "Stats endpoint missing rate_limit info"
    fi
}

test_solve_basic() {
    echo ""
    echo "=== Testing Solve Endpoint (Basic) ==="

    RESP=$(curl -s -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{
            "total_distance": 500,
            "tank_capacity": 100,
            "current_fuel": 20,
            "consumption_mpg": 10,
            "minimum_fuel": 10,
            "stations": [
                {"id": 1, "distance": 100, "price": 3.50},
                {"id": 2, "distance": 250, "price": 3.25},
                {"id": 3, "distance": 400, "price": 3.75}
            ]
        }')

    if echo "$RESP" | grep -q '"status": "OPTIMAL"'; then
        pass "Solve returns optimal solution"
    else
        fail "Solve did not return optimal solution"
        echo "  Response: $RESP"
    fi

    if echo "$RESP" | grep -q '"num_stops"'; then
        pass "Solve returns num_stops"
    else
        fail "Solve missing num_stops"
    fi

    if echo "$RESP" | grep -q '"total_cost"'; then
        pass "Solve returns total_cost"
    else
        fail "Solve missing total_cost"
    fi
}

test_solve_with_segments() {
    echo ""
    echo "=== Testing Solve with Segments ==="

    RESP=$(curl -s -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{
            "total_distance": 500,
            "tank_capacity": 100,
            "current_fuel": 30,
            "minimum_fuel": 10,
            "stations": [
                {"id": 1, "distance": 100, "price": 3.50},
                {"id": 2, "distance": 250, "price": 3.25},
                {"id": 3, "distance": 400, "price": 3.75}
            ],
            "segments": [
                {"start": 0, "mpg": 6.0},
                {"start": 200, "mpg": 8.0}
            ]
        }')

    if echo "$RESP" | grep -q '"status": "OPTIMAL"'; then
        pass "Solve with segments returns optimal solution"
    else
        fail "Solve with segments failed"
        echo "  Response: $RESP"
    fi
}

test_solve_infeasible() {
    echo ""
    echo "=== Testing Solve (Infeasible) ==="

    # Not enough fuel to reach first station
    RESP=$(curl -s -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{
            "total_distance": 500,
            "tank_capacity": 20,
            "current_fuel": 5,
            "consumption_mpg": 5,
            "minimum_fuel": 10,
            "stations": [
                {"id": 1, "distance": 200, "price": 3.50}
            ]
        }')

    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{
            "total_distance": 500,
            "tank_capacity": 20,
            "current_fuel": 5,
            "consumption_mpg": 5,
            "minimum_fuel": 10,
            "stations": [
                {"id": 1, "distance": 200, "price": 3.50}
            ]
        }')

    if [ "$HTTP_CODE" = "422" ] || echo "$RESP" | grep -qi "infeasible\|error"; then
        pass "Infeasible problem returns appropriate error"
    else
        fail "Infeasible problem should return error (got HTTP $HTTP_CODE)"
    fi
}

test_solve_bad_request() {
    echo ""
    echo "=== Testing Solve (Bad Request) ==="

    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d 'not valid json')

    if [ "$HTTP_CODE" = "400" ]; then
        pass "Invalid JSON returns 400"
    else
        fail "Invalid JSON should return 400 (got $HTTP_CODE)"
    fi

    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{}')

    if [ "$HTTP_CODE" = "400" ]; then
        pass "Empty object returns 400"
    else
        fail "Empty object should return 400 (got $HTTP_CODE)"
    fi
}

test_filter() {
    echo ""
    echo "=== Testing Filter Endpoint ==="

    RESP=$(curl -s -X POST http://localhost:$PORT/api/v1/filter \
        -H "Content-Type: application/json" \
        -d '{
            "stations": [
                {"id": 1, "lat": 34.0, "lon": -118.0, "price": 3.50},
                {"id": 2, "lat": 34.1, "lon": -117.9, "price": 3.25},
                {"id": 3, "lat": 40.0, "lon": -100.0, "price": 3.75}
            ],
            "route": [[34.0, -118.0], [34.05, -117.95], [34.1, -117.9]],
            "max_distance": 5
        }')

    if echo "$RESP" | grep -q '"count"'; then
        pass "Filter returns count"
    else
        fail "Filter missing count"
        echo "  Response: $RESP"
    fi
}

test_optimize() {
    echo ""
    echo "=== Testing Optimize Endpoint ==="

    RESP=$(curl -s -X POST http://localhost:$PORT/api/v1/optimize \
        -H "Content-Type: application/json" \
        -d '{
            "stations": [
                {"id": 1, "lat": 34.0, "lon": -118.0, "price": 3.50},
                {"id": 2, "lat": 34.5, "lon": -117.5, "price": 3.25}
            ],
            "route": [[34.0, -118.0], [34.25, -117.75], [34.5, -117.5]],
            "tank_capacity": 100,
            "current_fuel": 30,
            "consumption_mpg": 8,
            "minimum_fuel": 10,
            "max_distance": 10
        }')

    if echo "$RESP" | grep -q '"status"'; then
        pass "Optimize returns status"
    else
        fail "Optimize missing status"
        echo "  Response: $RESP"
    fi
}

test_cors() {
    echo ""
    echo "=== Testing CORS ==="

    RESP=$(curl -s -I -X OPTIONS http://localhost:$PORT/api/v1/health \
        -H "Origin: http://example.com" \
        -H "Access-Control-Request-Method: GET")

    if echo "$RESP" | grep -qi "access-control-allow"; then
        pass "CORS preflight returns appropriate headers"
    else
        fail "CORS headers missing"
    fi
}

# ============================================================================
# Rate Limiting Tests
# ============================================================================

test_rate_limiting() {
    echo ""
    echo "=== Testing Rate Limiting ==="

    stop_server
    start_server "--rate-limit-rps 2 --rate-limit-burst 3"

    # Rapid fire requests - should get some 429s
    ALLOWED=0
    DENIED=0
    for i in {1..10}; do
        CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://localhost:$PORT/api/v1/solve \
            -H "Content-Type: application/json" \
            -d '{"total_distance": 100, "tank_capacity": 50, "current_fuel": 20, "consumption_mpg": 10, "minimum_fuel": 5, "stations": [{"id": 1, "distance": 50, "price": 3.50}]}')
        if [ "$CODE" = "200" ]; then
            ALLOWED=$((ALLOWED + 1))
        elif [ "$CODE" = "429" ]; then
            DENIED=$((DENIED + 1))
        fi
    done

    if [ $ALLOWED -gt 0 ] && [ $DENIED -gt 0 ]; then
        pass "Rate limiting allows some requests and denies others (allowed=$ALLOWED, denied=$DENIED)"
    else
        fail "Rate limiting not working correctly (allowed=$ALLOWED, denied=$DENIED)"
    fi

    # Check stats show rate limiting
    STATS=$(curl -s http://localhost:$PORT/api/v1/stats)
    if echo "$STATS" | grep -q '"denied":'; then
        DENIED_COUNT=$(echo "$STATS" | grep -o '"denied": [0-9]*' | grep -o '[0-9]*')
        if [ "$DENIED_COUNT" -gt 0 ]; then
            pass "Stats show rate limit denials ($DENIED_COUNT)"
        else
            fail "Stats should show rate limit denials"
        fi
    fi

    # Test rate limit disabled
    stop_server
    start_server "--rate-limit-off"

    ALL_OK=1
    for i in {1..10}; do
        CODE=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:$PORT/api/v1/health)
        if [ "$CODE" != "200" ]; then
            ALL_OK=0
            break
        fi
    done

    if [ $ALL_OK -eq 1 ]; then
        pass "Rate limiting disabled allows all requests"
    else
        fail "With rate limiting disabled, all requests should succeed"
    fi
}

# ============================================================================
# Work Queue Tests
# ============================================================================

test_work_queue() {
    echo ""
    echo "=== Testing Work Queue ==="

    stop_server
    start_server "--queue-depth 5 --queue-timeout 2"

    # Check stats show work queue enabled
    STATS=$(curl -s http://localhost:$PORT/api/v1/stats)
    if echo "$STATS" | grep -q '"capacity": 5'; then
        pass "Work queue configured with correct depth"
    else
        fail "Work queue depth not configured correctly"
    fi

    # Test queue timeout tracking
    if echo "$STATS" | grep -q '"timeout_sec"'; then
        pass "Work queue shows timeout configuration"
    else
        fail "Work queue missing timeout configuration"
    fi

    # Test queue disabled
    stop_server
    start_server "--queue-off"

    STATS=$(curl -s http://localhost:$PORT/api/v1/stats)
    if echo "$STATS" | grep -q '"enabled": false'; then
        pass "Work queue can be disabled"
    else
        fail "Work queue should show disabled in stats"
    fi
}

# ============================================================================
# Main Test Runner
# ============================================================================

echo ""
echo "========================================"
echo "  FuelWise API Test Suite"
echo "========================================"

if [ $RATELIMIT_ONLY -eq 1 ]; then
    start_server ""
    test_rate_limiting
elif [ $QUEUE_ONLY -eq 1 ]; then
    start_server ""
    test_work_queue
else
    start_server ""

    # Basic tests
    test_health
    test_stats
    test_solve_basic
    test_cors

    if [ $QUICK -eq 0 ]; then
        # Extended tests
        test_solve_with_segments
        test_solve_infeasible
        test_solve_bad_request
        test_filter
        test_optimize
        test_rate_limiting
        test_work_queue
    fi
fi

stop_server

# Summary
echo ""
echo "========================================"
echo "  Test Summary"
echo "========================================"
echo "  Tests Run: $TESTS_RUN"
echo -e "  ${GREEN}Passed${NC}: $PASS"
if [ $FAIL -gt 0 ]; then
    echo -e "  ${RED}Failed${NC}: $FAIL"
    exit 1
else
    echo -e "  ${GREEN}All tests passed!${NC}"
    exit 0
fi
