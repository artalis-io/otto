#!/bin/bash
#
# API Audit Script
# Validates OTTO API modules against the transport-agnostic manifesto
#
# Usage:
#   ./scripts/api-audit.sh [module|all]
#   ./scripts/api-audit.sh velo          # Audit single module
#   ./scripts/api-audit.sh all           # Audit all modules
#   ./scripts/api-audit.sh --test        # Also run WASM demo tests
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG_FILE="$ROOT_DIR/site/api-config.json"
TEST_FILE="$ROOT_DIR/site/tests/wasm-demos.spec.js"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
PASS=0
FAIL=0
WARN=0

pass() {
    echo -e "  ${GREEN}✓${NC} $1"
    PASS=$((PASS + 1))
}

fail() {
    echo -e "  ${RED}✗${NC} $1"
    FAIL=$((FAIL + 1))
}

warn() {
    echo -e "  ${YELLOW}!${NC} $1"
    WARN=$((WARN + 1))
}

info() {
    echo -e "  ${BLUE}→${NC} $1"
}

# Check if jq is available
if ! command -v jq &> /dev/null; then
    echo "Error: jq is required but not installed"
    echo "Install with: brew install jq"
    exit 1
fi

# Parse arguments
TARGET="${1:-all}"
RUN_TESTS=false

for arg in "$@"; do
    if [[ "$arg" == "--test" ]]; then
        RUN_TESTS=true
    fi
done

# Get modules from api-config.json
get_modules() {
    if [[ "$TARGET" == "all" ]]; then
        jq -r '.modules[].id' "$CONFIG_FILE"
    else
        # Verify module exists
        if jq -e ".modules[] | select(.id == \"$TARGET\")" "$CONFIG_FILE" > /dev/null 2>&1; then
            echo "$TARGET"
        else
            echo "Error: Module '$TARGET' not found in api-config.json" >&2
            echo "Available modules:" >&2
            jq -r '.modules[].id' "$CONFIG_FILE" | sed 's/^/  /' >&2
            exit 1
        fi
    fi
}

# Extract prefix from header_file path (e.g., "carta/include/ct_api.h" -> "ct")
get_prefix() {
    local module="$1"
    local header_file
    header_file=$(jq -r ".modules[] | select(.id == \"$module\") | .header_file" "$CONFIG_FILE")
    basename "$header_file" | sed 's/_api\.h$//'
}

# Check if file exists
check_file() {
    local path="$1"
    local desc="$2"
    if [[ -f "$ROOT_DIR/$path" ]]; then
        pass "$desc: $path"
        return 0
    else
        fail "$desc: $path (not found)"
        return 1
    fi
}

# Count @api annotations in a header file
count_api_annotations() {
    local header="$1"
    grep -c '^\s*/\*@api' "$ROOT_DIR/$header" 2>/dev/null || echo 0
}

# Count @demo annotations in a header file
count_demo_annotations() {
    local header="$1"
    grep -c '@demo\s' "$ROOT_DIR/$header" 2>/dev/null || echo 0
}

# Get demo endpoint paths from header
get_demo_endpoints() {
    local header="$1"
    # Find @api blocks with @demo, extract the endpoint path
    # Note: awk uses POSIX regex - use [[:space:]] not \s
    awk '
        /\/\*@api/ { in_block=1; endpoint="" }
        in_block && /^[[:space:]]*\*[[:space:]]*(GET|POST|PUT|DELETE)[[:space:]]+/ {
            line=$0
            sub(/^[[:space:]]*\*[[:space:]]*/, "", line)
            split(line, parts, " ")
            endpoint = parts[2]
        }
        in_block && /@demo / && length(endpoint) > 0 { print endpoint }
        /\*\// { in_block=0 }
    ' "$ROOT_DIR/$header" 2>/dev/null
}

# Check if test file has tests for a module
check_test_coverage() {
    local module="$1"
    local header="$2"

    if [[ ! -f "$TEST_FILE" ]]; then
        warn "Test file not found: $TEST_FILE"
        return
    fi

    # Check if module has a test.describe block (case-insensitive)
    if grep -qi "test.describe.*$module" "$TEST_FILE" 2>/dev/null; then
        pass "Test suite exists for $module"
    else
        fail "No test suite for $module in wasm-demos.spec.js"
        return
    fi

    # Get demo endpoints and check each has a test
    local endpoints
    endpoints=$(get_demo_endpoints "$header")

    if [[ -z "$endpoints" ]]; then
        info "No @demo endpoints in $header"
        return
    fi

    local missing_tests=()
    while IFS= read -r endpoint; do
        # Normalize endpoint for grep (escape special chars, handle path params)
        local pattern
        # Use # as sed delimiter to avoid conflicts with / in paths
        pattern=$(echo "$endpoint" | sed 's#{[^}]*}#[^/]*#g' | sed 's#/#\\/#g')

        if grep -qE "fetch\(['\"]$pattern|fetch\(['\"][^'\"]*$pattern" "$TEST_FILE" 2>/dev/null; then
            pass "Test exists for $endpoint"
        else
            # Check for helper methods like .route() for velo
            if [[ "$module" == "velo" ]] && [[ "$endpoint" == *"/route"* ]]; then
                if grep -q "\.route(" "$TEST_FILE" 2>/dev/null; then
                    pass "Test exists for $endpoint (via .route() helper)"
                    continue
                fi
            fi
            missing_tests+=("$endpoint")
        fi
    done <<< "$endpoints"

    if [[ ${#missing_tests[@]} -gt 0 ]]; then
        for ep in "${missing_tests[@]}"; do
            fail "Missing test for $ep"
        done
    fi
}

# Check handler interface
check_handler_interface() {
    local module="$1"
    local prefix="$2"
    local header="$3"

    if [[ ! -f "$ROOT_DIR/$header" ]]; then
        return
    fi

    local PREFIX_UPPER
    PREFIX_UPPER=$(echo "$prefix" | tr '[:lower:]' '[:upper:]')

    # Check for required typedefs/structs
    if grep -q "${PREFIX_UPPER}APIContext\|${prefix}_api_context" "$ROOT_DIR/$header" 2>/dev/null; then
        pass "APIContext type defined"
    else
        fail "Missing ${PREFIX_UPPER}APIContext typedef"
    fi

    if grep -q "${PREFIX_UPPER}APIRequest\|${prefix}_api_request" "$ROOT_DIR/$header" 2>/dev/null; then
        pass "APIRequest struct defined"
    else
        fail "Missing ${PREFIX_UPPER}APIRequest struct"
    fi

    if grep -q "${PREFIX_UPPER}APIResponse\|${prefix}_api_response" "$ROOT_DIR/$header" 2>/dev/null; then
        pass "APIResponse struct defined"
    else
        fail "Missing ${PREFIX_UPPER}APIResponse struct"
    fi

    # Check for handler function
    if grep -q "${prefix}_api_handle\|${prefix}_handle_request" "$ROOT_DIR/$header" 2>/dev/null; then
        pass "Handler function declared"
    else
        fail "Missing ${prefix}_api_handle() function"
    fi
}

# Check WASM configuration
check_wasm_config() {
    local module="$1"
    local prefix="$2"

    local wasm_enabled
    wasm_enabled=$(jq -r ".modules[] | select(.id == \"$module\") | .wasm.enabled" "$CONFIG_FILE")

    if [[ "$wasm_enabled" != "true" ]]; then
        info "WASM not enabled for $module"
        return
    fi

    # Check WASM wrapper exists
    local wasm_file="$module/wasm/${prefix}_wasm_api.c"
    if [[ -f "$ROOT_DIR/$wasm_file" ]]; then
        pass "WASM wrapper: $wasm_file"
    else
        # Try alternate naming
        wasm_file="$module/wasm/src/${prefix}_wasm_api.c"
        if [[ -f "$ROOT_DIR/$wasm_file" ]]; then
            pass "WASM wrapper: $wasm_file"
        else
            fail "WASM wrapper not found"
        fi
    fi

    # Check handlers file exists
    local handlers_file
    handlers_file=$(jq -r ".modules[] | select(.id == \"$module\") | .wasm.handlers_file" "$CONFIG_FILE")
    if [[ "$handlers_file" != "null" ]] && [[ -n "$handlers_file" ]]; then
        if [[ -f "$ROOT_DIR/site/$handlers_file" ]]; then
            pass "Handlers file: site/$handlers_file"
        else
            fail "Handlers file not found: site/$handlers_file"
        fi
    fi
}

# Audit a single module
audit_module() {
    local module="$1"
    echo ""
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}  Module: ${NC}${module}"
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

    local prefix header_file
    prefix=$(get_prefix "$module")
    header_file=$(jq -r ".modules[] | select(.id == \"$module\") | .header_file" "$CONFIG_FILE")

    echo ""
    echo "Structure:"
    check_file "$header_file" "Handler header"

    local impl_file="$module/src/${prefix}_api.c"
    check_file "$impl_file" "Handler implementation" || true

    local server_file="$module/api/src/main.c"
    check_file "$server_file" "Server main" || true

    echo ""
    echo "Handler Interface:"
    check_handler_interface "$module" "$prefix" "$header_file"

    echo ""
    echo "Annotations:"
    local api_count demo_count
    api_count=$(count_api_annotations "$header_file")
    demo_count=$(count_demo_annotations "$header_file")

    if [[ "$api_count" -gt 0 ]]; then
        pass "$api_count @api annotations found"
    else
        fail "No @api annotations in $header_file"
    fi

    if [[ "$demo_count" -gt 0 ]]; then
        pass "$demo_count @demo annotations found"
    else
        warn "No @demo annotations (WASM demos won't be generated)"
    fi

    echo ""
    echo "WASM Configuration:"
    check_wasm_config "$module" "$prefix"

    echo ""
    echo "Test Coverage:"
    check_test_coverage "$module" "$header_file"
}

# Main
echo ""
echo -e "${BLUE}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║              OTTO API Audit                               ║${NC}"
echo -e "${BLUE}╚═══════════════════════════════════════════════════════════╝${NC}"

# Get and audit modules
MODULES=$(get_modules)

for module in $MODULES; do
    audit_module "$module"
done

# Run tests if requested
if [[ "$RUN_TESTS" == true ]]; then
    echo ""
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}  Running WASM Demo Tests${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
    echo ""

    if make -C "$ROOT_DIR" test-api-docs; then
        pass "All WASM demo tests passed"
    else
        fail "WASM demo tests failed"
    fi
fi

# Check api-docs are up to date
echo ""
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  Documentation Status${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo ""

if make -C "$ROOT_DIR" api-docs-check 2>/dev/null; then
    pass "api.html is up to date"
else
    fail "api.html needs regeneration (run: make api-docs)"
fi

# Summary
echo ""
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  Summary${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  ${GREEN}Passed:${NC}   $PASS"
echo -e "  ${RED}Failed:${NC}   $FAIL"
echo -e "  ${YELLOW}Warnings:${NC} $WARN"
echo ""

if [[ $FAIL -gt 0 ]]; then
    echo -e "${RED}Audit FAILED${NC}"
    exit 1
else
    echo -e "${GREEN}Audit PASSED${NC}"
    exit 0
fi
