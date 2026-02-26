#!/bin/bash
#
# Surge API Test Suite
#
# Tests health, version, solve, error handling, metrics, and CORS endpoints
#

set -e

PORT=8098
PASS=0
FAIL=0
TESTS_RUN=0

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

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
    ./surge-solver -p $PORT $extra_args > /dev/null 2>&1 &
    SERVER_PID=$!
    sleep 1

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

trap stop_server EXIT

# ============================================================================
# Health & Version
# ============================================================================

test_health() {
    echo ""
    echo "=== Testing Health Endpoint ==="

    RESP=$(curl -s http://localhost:$PORT/api/v1/health)

    if echo "$RESP" | grep -qE '"status"'; then
        pass "Health endpoint returns status"
    else
        fail "Health endpoint missing status"
    fi

    if echo "$RESP" | grep -qE '"service":\s*"surge"'; then
        pass "Health endpoint returns correct service name"
    else
        fail "Health endpoint missing service name"
    fi
}

test_version() {
    echo ""
    echo "=== Testing Version Endpoint ==="

    RESP=$(curl -s http://localhost:$PORT/api/v1/version)

    if echo "$RESP" | grep -qE '"version"'; then
        pass "Version endpoint returns version"
    else
        fail "Version endpoint missing version"
    fi
}

# ============================================================================
# Solve Endpoint
# ============================================================================

test_solve_basic() {
    echo ""
    echo "=== Testing Solve (Basic Delivery) ==="

    RESP=$(curl -s -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{
            "config": {"max_iterations": 100, "seed": 42, "deterministic": true},
            "depots": [{"x": 0, "y": 0, "tw_early": 0, "tw_late": 99999}],
            "vehicles": [{
                "start_depot_id": 0, "end_depot_id": 0,
                "shift_early": 0, "shift_late": 99999,
                "capacity": [100]
            }],
            "tasks": [{"type": "delivery", "x": 10, "y": 0,
                "tw_early": 0, "tw_late": 99999, "service_seconds": 0,
                "demand": [-1]}],
            "requests": [{"delivery_task_id": 0}]
        }')

    if echo "$RESP" | grep -qE '"unassigned":\s*0'; then
        pass "Basic delivery: all requests assigned"
    else
        fail "Basic delivery: unassigned != 0"
        echo "  Response: $RESP"
    fi

    if echo "$RESP" | grep -qE '"routes"'; then
        pass "Basic delivery: routes present in response"
    else
        fail "Basic delivery: routes missing"
    fi
}

test_solve_pd() {
    echo ""
    echo "=== Testing Solve (Pickup-Delivery) ==="

    RESP=$(curl -s -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{
            "config": {"max_iterations": 100, "seed": 42, "deterministic": true},
            "depots": [{"x": 0, "y": 0, "tw_early": 0, "tw_late": 99999}],
            "vehicles": [{
                "start_depot_id": 0, "end_depot_id": 0,
                "shift_early": 0, "shift_late": 99999,
                "capacity": [100]
            }],
            "tasks": [
                {"type": "pickup", "x": 5, "y": 0,
                 "tw_early": 0, "tw_late": 99999, "service_seconds": 0,
                 "demand": [1]},
                {"type": "delivery", "x": 15, "y": 0,
                 "tw_early": 0, "tw_late": 99999, "service_seconds": 0,
                 "demand": [-1]}
            ],
            "requests": [{"pickup_task_id": 0, "delivery_task_id": 1}]
        }')

    if echo "$RESP" | grep -qE '"unassigned":\s*0'; then
        pass "PD request: all requests assigned"
    else
        fail "PD request: unassigned != 0"
        echo "  Response: $RESP"
    fi
}

test_solve_with_profiles() {
    echo ""
    echo "=== Testing Solve (Travel Profiles) ==="

    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '{
            "config": {"max_iterations": 100, "seed": 42, "deterministic": true},
            "locations": [{"x": 0, "y": 0}, {"x": 10, "y": 0}],
            "speed_profiles": [{"entries": [{"start_time": 0, "multiplier": 1.0}]}],
            "travel_profiles": [{"distance_matrix": [0,10,10,0], "duration_matrix": [0,10,10,0]}],
            "depots": [{"location_id": 0, "tw_early": 0, "tw_late": 99999}],
            "vehicles": [{"start_depot_id": 0, "end_depot_id": 0, "shift_early": 0, "shift_late": 99999, "capacity": [100], "travel_profile_id": 0}],
            "tasks": [{"type": "delivery", "location_id": 1, "tw_early": 0, "tw_late": 99999, "service_seconds": 0, "demand": [-1]}],
            "requests": [{"delivery_task_id": 0}]
        }')

    if [ "$HTTP_CODE" = "200" ]; then
        pass "Travel profiles: solve returns 200"
    else
        fail "Travel profiles: expected 200, got $HTTP_CODE"
    fi
}

# ============================================================================
# Error Handling
# ============================================================================

test_error_handling() {
    echo ""
    echo "=== Testing Error Handling ==="

    # Empty body
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d '')

    if [ "$HTTP_CODE" = "400" ]; then
        pass "Empty body returns 400"
    else
        fail "Empty body should return 400 (got $HTTP_CODE)"
    fi

    # Malformed JSON
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST http://localhost:$PORT/api/v1/solve \
        -H "Content-Type: application/json" \
        -d 'not valid json')

    if [ "$HTTP_CODE" = "400" ]; then
        pass "Malformed JSON returns 400"
    else
        fail "Malformed JSON should return 400 (got $HTTP_CODE)"
    fi
}

# ============================================================================
# Metrics & CORS
# ============================================================================

test_metrics() {
    echo ""
    echo "=== Testing Metrics Endpoint ==="

    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:$PORT/metrics)

    if [ "$HTTP_CODE" = "200" ]; then
        pass "Metrics endpoint returns 200"
    else
        fail "Metrics endpoint should return 200 (got $HTTP_CODE)"
    fi
}

test_cors() {
    echo ""
    echo "=== Testing CORS ==="

    RESP=$(curl -s -I -X OPTIONS http://localhost:$PORT/api/v1/solve \
        -H "Origin: http://example.com" \
        -H "Access-Control-Request-Method: POST")

    if echo "$RESP" | grep -qi "access-control-allow"; then
        pass "CORS preflight returns appropriate headers"
    else
        fail "CORS headers missing"
    fi
}

# ============================================================================
# Main Test Runner
# ============================================================================

echo ""
echo "========================================"
echo "  Surge API Test Suite"
echo "========================================"

start_server ""

test_health
test_version
test_solve_basic
test_solve_pd
test_solve_with_profiles
test_error_handling
test_metrics
test_cors

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
