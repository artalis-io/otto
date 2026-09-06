#!/usr/bin/env bash
#
# Carta Tile Server API test suite.
#
# Starts carta-tile-server against the Monaco PBF and exercises the endpoints.
# Exits non-zero on any failure, so it can gate CI.
#
# Usage:  bash carta/api/test_api.sh [pbf-file]
#
set -u

PORT=8481
PBF="${1:-data/monaco-latest.osm.pbf}"
SERVER=./carta/api/carta-tile-server

# A Monaco tile at z14 (Monaco is around 43.73N, 7.42E)
Z=14; X=8529; Y=5975

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

if [ ! -f "$PBF" ]; then
    echo "ERROR: PBF not found: $PBF (run: make data/monaco-latest.osm.pbf)"
    exit 1
fi
if [ ! -x "$SERVER" ]; then
    echo "ERROR: server not built: $SERVER (run: make carta-api)"
    exit 1
fi

"$SERVER" -p $PORT "$PBF" > /tmp/carta_test_server.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# Wait for it to come up (PBF parse takes a moment)
for _ in $(seq 1 60); do
    if curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null; then break; fi
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "ERROR: server exited during startup"
        cat /tmp/carta_test_server.log
        exit 1
    fi
    sleep 0.5
done

echo ""
echo "========================================"
echo "  Carta API Test Suite"
echo "========================================"

echo ""
echo "=== Health / Stats / TileJSON ==="
RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/health")
check "Health reports healthy" "$RESP" '"status": *"healthy"'

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/stats")
check "Stats returns work_queue info" "$RESP" '"work_queue"'

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/tiles.json")
check "TileJSON returns tiles array" "$RESP" '"tiles"'

echo ""
echo "=== Tiles ==="
CODE=$(curl -s -o /tmp/carta_tile.mvt -w '%{http_code}' -m 30 \
    "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.mvt")
check_code "MVT tile returns 200" "$CODE" "200"

CT=$(curl -s -o /dev/null -w '%{content_type}' -m 30 \
    "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.mvt")
check "MVT tile has vector-tile content type" "$CT" 'vnd.mapbox-vector-tile'

CODE=$(curl -s -o /tmp/carta_tile.png -w '%{http_code}' -m 30 \
    "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.png")
check_code "PNG tile returns 200" "$CODE" "200"

if head -c 8 /tmp/carta_tile.png | grep -q 'PNG'; then
    pass "PNG tile has a PNG signature"
else
    fail "PNG tile has a PNG signature"
fi

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 30 \
    "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.txt?width=40")
check_code "ASCII tile returns 200" "$CODE" "200"

echo ""
echo "=== ETag / conditional request ==="
ETAG=$(curl -s -D - -o /dev/null -m 30 "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.png" \
    | grep -i '^etag:' | tr -d '\r' | sed 's/^[Ee][Tt][Aa][Gg]: *//')
if [ -n "$ETAG" ]; then
    pass "PNG tile returns an ETag"
    CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 30 \
        -H "If-None-Match: $ETAG" "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.png")
    check_code "Matching If-None-Match returns 304" "$CODE" "304"
else
    fail "PNG tile returns an ETag"
fi

echo ""
echo "=== Error handling ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 \
    "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.bogus")
check_code "Unknown tile format returns 400" "$CODE" "400"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/tiles/99/0/0.png")
check_code "Zoom out of range returns 400" "$CODE" "400"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/tiles/garbage")
check_code "Malformed tile path returns 400" "$CODE" "400"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/nope")
check_code "Unknown path returns 404" "$CODE" "404"

echo ""
echo "=== CORS ==="
RESP=$(curl -s -i -m 5 -X OPTIONS "http://127.0.0.1:$PORT/tiles.json" -H "Origin: https://example.com")
check "Preflight returns 204" "$RESP" '204'
check "Preflight sets Allow-Origin" "$RESP" 'Access-Control-Allow-Origin'

RESP=$(curl -s -D - -o /dev/null -m 30 "http://127.0.0.1:$PORT/tiles/$Z/$X/$Y.mvt")
check "Tile response carries CORS header" "$RESP" 'Access-Control-Allow-Origin'

echo ""
echo "=== Async dispatch (KlAsyncOp + KlThreadPool) ==="
# NOTE: collect the curl PIDs and wait only on those. A bare `wait` would also
# wait on the server started above with &, which never exits.
BURST_PIDS=""
for n in 1 2 3 4 5; do
    curl -s -m 30 -o /dev/null "http://127.0.0.1:$PORT/tiles/$Z/$X/$((Y + n)).png" &
    BURST_PIDS="$BURST_PIDS $!"
done
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/health")
for pid in $BURST_PIDS; do wait "$pid" 2>/dev/null || true; done
check_code "Health stays responsive during concurrent renders" "$CODE" "200"

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/stats")
if echo "$RESP" | grep -qE '"expired": *0'; then
    pass "No renders expired"
else
    fail "No renders expired" "$RESP"
fi

echo ""
echo "========================================"
echo "  Test Summary"
echo "========================================"
echo "  Tests Run: $((PASS + FAIL))"
echo "  Passed: $PASS"
if [ $FAIL -gt 0 ]; then
    echo "  Failed: $FAIL"
    echo ""
    if kill -0 $SERVER_PID 2>/dev/null; then
        echo "--- server still running ---"
    else
        echo "--- SERVER IS NOT RUNNING (crashed or exited) ---"
    fi
    echo "--- server log ---"
    tail -40 /tmp/carta_test_server.log
    exit 1
fi
echo "  All tests passed!"
exit 0
