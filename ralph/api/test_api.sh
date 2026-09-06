#!/usr/bin/env bash
#
# Ralph LP/MIP Solver API test suite.
#
# Starts ralph-solver-server and exercises the endpoints. Exits non-zero on any
# failure, so it can gate CI. Needs no external data.
#
# Usage:  bash ralph/api/test_api.sh
#
set -u

PORT=8485
SERVER=./ralph/api/ralph-solver-server

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

if [ ! -x "$SERVER" ]; then
    echo "ERROR: server not built: $SERVER (run: make ralph-api)"
    exit 1
fi

"$SERVER" -p $PORT > /tmp/ralph_test_server.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

for _ in $(seq 1 40); do
    if curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null; then break; fi
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "ERROR: server exited during startup"
        cat /tmp/ralph_test_server.log
        exit 1
    fi
    sleep 0.5
done

echo ""
echo "========================================"
echo "  Ralph API Test Suite"
echo "========================================"

echo ""
echo "=== Health / Formats ==="
RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/health")
check "Health reports ok" "$RESP" '"status":"ok"'

RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/formats")
check "Formats lists lp" "$RESP" '"lp"'
check "Formats lists mps" "$RESP" '"mps"'

echo ""
echo "=== Solve ==="
# max 5x + 3y s.t. 2x + 4y <= 40, 3x + 2y <= 24  ->  x=8, y=0, objective 40
SOLVE=$(curl -s -m 20 -X POST "http://127.0.0.1:$PORT/api/v1/solve" \
    -H "Content-Type: application/json" \
    -d '{"format":"lp","problem":"max: 5 x + 3 y\nsubject to\nwood: 2 x + 4 y <= 40\nlabor: 3 x + 2 y <= 24\nbounds\nx >= 0\ny >= 0\nend"}')

# NOTE: the API is documented as lowercase in ralph/api/CLAUDE.md and
# ralph/wasm/CLAUDE.md, and status_to_json() says "lowercase for JSON".
# The old Makefile target grepped for "OPTIMAL" and so always printed FAIL.
check "Solve reports optimal" "$SOLVE" '"status":"optimal"'
check "Solve returns the right objective" "$SOLVE" '"objective":40'
check "Solve returns variables" "$SOLVE" '"variables"'
check "Solve reports iterations" "$SOLVE" '"iterations"'

echo ""
echo "=== Error handling ==="
RESP=$(curl -s -m 10 -X POST "http://127.0.0.1:$PORT/api/v1/solve" \
    -H "Content-Type: application/json" -d '{"format":"lp","problem":"invalid syntax"}')
check "Invalid LP returns an error" "$RESP" '"error"'

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 -X POST \
    "http://127.0.0.1:$PORT/api/v1/solve" -H "Content-Type: application/json" -d '{not json')
if [ "$CODE" = "400" ] || [ "$CODE" = "200" ]; then
    pass "Malformed JSON handled without crashing (got $CODE)"
else
    fail "Malformed JSON handled without crashing" "got $CODE"
fi

CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/nope")
check_code "Unknown path returns 404" "$CODE" "404"

# Ralph dispatches every path through ralph_api_handle(), which answers 404 for
# a wrong method rather than 405 -- same as the mongoose server did.
CODE=$(curl -s -o /dev/null -w '%{http_code}' -m 10 "http://127.0.0.1:$PORT/api/v1/solve")
check_code "GET on /solve returns 404" "$CODE" "404"

echo ""
echo "=== CORS ==="
RESP=$(curl -s -i -m 5 -X OPTIONS "http://127.0.0.1:$PORT/api/v1/solve")
check "Preflight returns 204" "$RESP" '204'
check "Preflight sets Allow-Origin" "$RESP" 'Access-Control-Allow-Origin'

RESP=$(curl -s -D - -o /dev/null -m 5 "http://127.0.0.1:$PORT/api/v1/health")
check "Responses carry CORS header" "$RESP" 'Access-Control-Allow-Origin'

echo ""
echo "=== Query string passthrough ==="
RESP=$(curl -s -m 5 "http://127.0.0.1:$PORT/api/v1/health?foo=bar")
check "Query string does not break routing" "$RESP" '"status":"ok"'

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
    tail -40 /tmp/ralph_test_server.log
    exit 1
fi
echo "  All tests passed!"
exit 0
