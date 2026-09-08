#!/usr/bin/env bash
#
# Locus Geocoder API test suite.
#
# Starts locus-geocoder against the Monaco PBF and exercises the endpoints.
# Exits non-zero on any failure, so it can gate CI.
#
# Usage:  bash locus/api/test_api.sh [pbf-file]
#
set -u

PORT=8483
PBF="${1:-data/monaco-latest.osm.pbf}"
SERVER=./locus/api/locus-geocoder

# Monaco: around 43.7384 N, 7.4246 E
LAT=43.7384
LON=7.4246

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
    echo "ERROR: server not built: $SERVER (run: make locus-api)"
    exit 1
fi

"$SERVER" -p $PORT "$PBF" > /tmp/locus_test_server.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# Wait for it to come up (PBF parse + index build take a moment)
for _ in $(seq 1 60); do
    if curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null; then break; fi
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "ERROR: server exited during startup"
        cat /tmp/locus_test_server.log
        exit 1
    fi
    sleep 0.5
done

echo ""
echo "========================================"
echo "  Locus API Test Suite"
echo "========================================"

echo ""
echo "=== Health / Stats ==="
RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/health")
check "Health reports healthy" "$RESP" '"status": *"healthy"'
check "Health reports service name" "$RESP" 'locus-geocoder'

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/stats")
check "Stats returns work_queue info" "$RESP" '"work_queue"'

echo ""
echo "=== Search ==="
RESP=$(curl -s -m 20 "http://127.0.0.1:$PORT/api/v1/search?q=Monaco&limit=5")
check "Search returns results array" "$RESP" '"results"'

RESP=$(curl -s -m 20 "http://127.0.0.1:$PORT/api/v1/search?q=zzzzzzzznotfound")
check "Search with no matches still returns results" "$RESP" '"results"'

echo ""
echo "=== Autocomplete ==="
# Autocomplete returns a bare JSON array of suggestion strings, not an object
# with a "results" key.
RESP=$(curl -s -m 20 "http://127.0.0.1:$PORT/api/v1/autocomplete?q=Mon&limit=5")
check "Autocomplete returns a JSON array" "$RESP" '^\['

# Assert autocomplete against a name the index actually holds rather than
# against a hardcoded one. data/monaco-latest.osm.pbf is re-downloaded from
# Geofabrik on every run, so its contents drift: "Mon" with limit=5 used to
# surface "Monaco" and later did not, which failed this suite for a reason
# that had nothing to do with the server. Take the first name search returns,
# feed its 4-character prefix back to autocomplete, and require that name
# back -- that tests the trie's prefix behaviour and holds for any extract.
NAME=$(curl -s -m 20 "http://127.0.0.1:$PORT/api/v1/search?q=Monaco&limit=5" \
       | grep -oE '"name"[[:space:]]*:[[:space:]]*"[^"]*"' \
       | sed 's/.*"\(.*\)"$/\1/' | grep -v '^$' | head -1)
if [ -n "$NAME" ]; then
    PREFIX=$(printf '%s' "$NAME" | cut -c1-4)
    RESP=$(curl -s -m 20 --get --data-urlencode "q=$PREFIX" --data-urlencode "limit=25" \
           "http://127.0.0.1:$PORT/api/v1/autocomplete")
    check "Autocomplete on a known name's prefix returns that name" \
          "$RESP" "$(printf '%s' "$NAME" | sed 's/[][\.*^$/]/\\&/g')"
else
    fail "Search returned a usable name to drive the autocomplete check"
fi

echo ""
echo "=== Reverse ==="
RESP=$(curl -s -m 20 "http://127.0.0.1:$PORT/api/v1/reverse?lat=$LAT&lon=$LON")
check "Reverse returns a JSON body" "$RESP" '{'

echo ""
echo "=== Error handling ==="
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/search")
check_code "Search without 'q' returns 400" "$CODE" "400"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/autocomplete")
check_code "Autocomplete without 'q' returns 400" "$CODE" "400"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/reverse?lat=$LAT")
check_code "Reverse without 'lon' returns 400" "$CODE" "400"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 \
    "http://127.0.0.1:$PORT/api/v1/reverse?lat=999&lon=999")
check_code "Reverse with invalid coordinates returns 400" "$CODE" "400"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/nope")
check_code "Unknown path returns 404" "$CODE" "404"

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 -X DELETE "http://127.0.0.1:$PORT/api/v1/search")
check_code "Wrong method returns 405" "$CODE" "405"

echo ""
echo "=== CORS ==="
RESP=$(curl -s -i -m 5 -X OPTIONS "http://127.0.0.1:$PORT/api/v1/search" -H "Origin: https://example.com")
check "Preflight returns 204" "$RESP" '204'
check "Preflight sets Allow-Origin" "$RESP" 'Access-Control-Allow-Origin'

echo ""
echo "=== Async dispatch (KlAsyncOp + KlThreadPool) ==="
# NOTE: collect the curl PIDs and wait only on those. A bare `wait` would also
# wait on the server started above with &, which never exits.
BURST_PIDS=""
for _ in 1 2 3 4 5; do
    curl -s -m 20 -o /dev/null "http://127.0.0.1:$PORT/api/v1/search?q=Monaco" &
    BURST_PIDS="$BURST_PIDS $!"
done
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/health")
for pid in $BURST_PIDS; do wait "$pid" 2>/dev/null || true; done
check_code "Health stays responsive during concurrent searches" "$CODE" "200"

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/stats")
if echo "$RESP" | grep -qE '"pushed": *[1-9]'; then
    pass "Searches dispatched through the thread pool"
else
    fail "Searches dispatched through the thread pool" "$RESP"
fi
if echo "$RESP" | grep -qE '"expired": *0'; then
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
    echo ""
    if kill -0 $SERVER_PID 2>/dev/null; then
        echo "--- server still running ---"
    else
        echo "--- SERVER IS NOT RUNNING (crashed or exited) ---"
    fi
    echo "--- server log ---"
    tail -40 /tmp/locus_test_server.log
    exit 1
fi
echo "  All tests passed!"
exit 0
