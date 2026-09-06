#!/usr/bin/env bash
#
# Velo Route Server API test suite.
#
# Starts velo-route-server against the Monaco graph and exercises the
# endpoints. Exits non-zero on any failure, so it can gate CI.
#
# Usage:  bash velo/api/test_api.sh [graph-file]
#
set -u

PORT=8391
GRAPH="${1:-data/monaco.vlg}"
SERVER=./velo/api/velo-route-server

# Monaco coordinates, inside the graph bbox (43.7232,7.4054)-(43.7544,7.4447)
FROM="43.7384,7.4246"
TO="43.7311,7.4197"

PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; [ $# -gt 1 ] && echo "        got: $2"; }

check() {  # label, response, expected-substring
    if echo "$2" | grep -q "$3"; then pass "$1"; else fail "$1" "$(echo "$2" | head -c 200)"; fi
}

check_code() {  # label, actual, expected
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "expected $3, got $2"; fi
}

if [ ! -f "$GRAPH" ]; then
    echo "ERROR: graph not found: $GRAPH (run: make data/monaco.vlg)"
    exit 1
fi
if [ ! -x "$SERVER" ]; then
    echo "ERROR: server not built: $SERVER (run: make velo-api)"
    exit 1
fi

"$SERVER" -p $PORT "$GRAPH" > /tmp/velo_test_server.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# Wait for it to come up (graph load + landmarks take a moment)
for _ in $(seq 1 40); do
    if curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null; then break; fi
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "ERROR: server exited during startup"
        cat /tmp/velo_test_server.log
        exit 1
    fi
    sleep 0.5
done

echo ""
echo "========================================"
echo "  Velo API Test Suite"
echo "========================================"

echo ""
echo "=== Health / Stats ==="
RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/health")
check "Health reports healthy" "$RESP" '"status": *"healthy"'
check "Health reports service name" "$RESP" 'velo-route-server'

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/stats")
check "Stats returns graph node count" "$RESP" '"num_nodes"'
check "Stats returns work_queue info" "$RESP" '"work_queue"'
check "Stats returns bbox" "$RESP" '"bbox"'

echo ""
echo "=== GET /api/v1/route (query string) ==="
RESP=$(curl -s -m 20 "http://127.0.0.1:$PORT/api/v1/route?from=$FROM&to=$TO&profile=car&mode=fastest&geometry=true")
check "GET route succeeds" "$RESP" '"status":"ok"'
check "GET route returns distance" "$RESP" '"distance"'
check "GET route returns geometry" "$RESP" '"geometry"'
check "GET route echoes profile" "$RESP" '"profile":"car"'

RESP=$(curl -s -m 20 "http://127.0.0.1:$PORT/api/v1/route?from=$FROM&to=$TO&geometry=false")
check "geometry=false omits geometry" "$RESP" '"status":"ok"'
if echo "$RESP" | grep -q '"geometry"'; then
    fail "geometry=false actually omits the key" "$RESP"
else
    pass "geometry=false actually omits the key"
fi

echo ""
echo "=== POST /api/v1/route (JSON body) ==="
RESP=$(curl -s -m 20 -X POST "http://127.0.0.1:$PORT/api/v1/route" \
    -H "Content-Type: application/json" \
    -d "{\"from\":\"$FROM\",\"to\":\"$TO\",\"profile\":\"truck\",\"mode\":\"shortest\"}")
check "POST route succeeds" "$RESP" '"status":"ok"'
check "POST honours profile from JSON" "$RESP" '"profile":"truck"'
check "POST honours mode from JSON" "$RESP" '"mode":"shortest"'

RESP=$(curl -s -m 20 -X POST "http://127.0.0.1:$PORT/api/v1/route" \
    -H "Content-Type: application/json" \
    -d "{\"from\":\"$FROM\",\"to\":\"$TO\",\"geometry\":false}")
if echo "$RESP" | grep -q '"geometry"'; then
    fail "POST honours geometry:false" "$RESP"
else
    pass "POST honours geometry:false"
fi

echo ""
echo "=== Error handling ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 5 "http://127.0.0.1:$PORT/api/v1/route?to=$TO")
check_code "Missing 'from' returns 400" "$CODE" "400"

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/route?from=garbage&to=$TO")
check "Invalid coordinate rejected" "$RESP" '"error"'

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/route?from=0,0&to=$TO")
check "Out-of-bounds origin rejected" "$RESP" 'outside graph bounds'

RESP=$(curl -s -m 5 -X POST "http://127.0.0.1:$PORT/api/v1/route" \
    -H "Content-Type: application/json" -d '{not json')
check "Malformed JSON body rejected" "$RESP" '"error"'

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 5 "http://127.0.0.1:$PORT/api/v1/nope")
check_code "Unknown path returns 404" "$CODE" "404"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 5 -X DELETE "http://127.0.0.1:$PORT/api/v1/route")
check_code "Wrong method returns 405" "$CODE" "405"

echo ""
echo "=== CORS ==="
RESP=$(curl -s -i -m 5 -X OPTIONS "http://127.0.0.1:$PORT/api/v1/route" -H "Origin: https://example.com")
check "Preflight returns 204" "$RESP" '204'
check "Preflight sets Allow-Origin" "$RESP" 'Access-Control-Allow-Origin'

echo ""
echo "=== Async dispatch (KlAsyncOp + KlThreadPool) ==="
# NOTE: collect the curl PIDs and wait only on those. A bare `wait` would
# also wait on the server started above with &, which never exits.
BURST_PIDS=""
for _ in 1 2 3 4 5; do
    curl -s -m 20 -o /dev/null "http://127.0.0.1:$PORT/api/v1/route?from=$FROM&to=$TO" &
    BURST_PIDS="$BURST_PIDS $!"
done
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/health")
for pid in $BURST_PIDS; do wait "$pid" 2>/dev/null || true; done
check_code "Health stays responsive during concurrent routes" "$CODE" "200"

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/stats")
if echo "$RESP" | grep -qE '"pushed":[1-9]'; then
    pass "Routes dispatched through the thread pool"
else
    fail "Routes dispatched through the thread pool" "$RESP"
fi
if echo "$RESP" | grep -qE '"expired":0'; then
    pass "No requests expired"
else
    fail "No requests expired" "$RESP"
fi

echo ""
echo "========================================"
echo "  Test Summary"
echo "========================================"
echo "  Tests Run: $((PASS + FAIL))"
echo "  Passed: $PASS"
if [ $FAIL -gt 0 ]; then
    echo "  Failed: $FAIL"
    exit 1
fi
echo "  All tests passed!"
exit 0
