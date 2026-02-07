#!/bin/bash
#
# CI/CD Pipeline Script for FuelWise Platform
#
# Usage:
#   ./scripts/ci.sh [stage]
#   ./scripts/ci.sh           # Run all stages
#   ./scripts/ci.sh build     # Build only
#   ./scripts/ci.sh test      # Test only
#   ./scripts/ci.sh lint      # Lint only
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[PASS]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }

STAGE_PASSED=0
STAGE_FAILED=0
STAGE_SKIPPED=0

run_stage() {
    local name="$1"
    local cmd="$2"
    
    echo ""
    echo "=========================================="
    echo "Stage: $name"
    echo "=========================================="
    
    if eval "$cmd"; then
        success "$name passed"
        ((STAGE_PASSED++))
    else
        error "$name failed"
        ((STAGE_FAILED++))
    fi
}

skip_stage() {
    local name="$1"
    local reason="$2"
    
    echo ""
    echo "=========================================="
    echo "Stage: $name (SKIPPED)"
    echo "Reason: $reason"
    echo "=========================================="
    
    ((STAGE_SKIPPED++))
}

# Stage: Clean
stage_clean() {
    info "Cleaning build artifacts..."
    make clean
}

# Stage: Build Libraries
stage_build() {
    info "Building all libraries..."
    make all
}

# Stage: Build APIs
stage_build_api() {
    info "Building API servers..."
    make fuelwise-api
    make velo-api
    make carta-api
}

# Stage: Unit Tests
stage_test() {
    info "Running unit tests..."
    make test
}

# Stage: API Tests (requires data)
stage_test_api() {
    if [[ -f data/hungary-latest.osm.pbf ]]; then
        info "Running API tests..."
        make test-fuelwise-api
        # Note: velo and carta tests take longer due to PBF loading
        # make test-velo-api
        # make test-carta-api
    else
        warn "Skipping API tests (no OSM data)"
        warn "Run: ./scripts/download-osm.sh monaco"
        return 0
    fi
}

# Stage: Check Generated Files
stage_check_generated() {
    info "Checking generated files are up-to-date..."
    make api-docs-check
}

# Stage: C Lint (basic checks)
stage_lint_c() {
    info "Checking C code style..."
    
    local issues=0
    
    # Check for tabs (we use 4 spaces)
    if grep -rn $'\t' --include="*.c" --include="*.h" ralph/ fuelwise/ velo/ carta/ shared/ 2>/dev/null | head -5; then
        warn "Found tabs in source files (should use spaces)"
        ((issues++))
    fi
    
    # Check for trailing whitespace
    if grep -rn ' $' --include="*.c" --include="*.h" ralph/ fuelwise/ velo/ carta/ shared/ 2>/dev/null | head -5; then
        warn "Found trailing whitespace"
        ((issues++))
    fi
    
    # Check for very long lines (>120 chars)
    local long_lines=$(find ralph/ fuelwise/ velo/ carta/ shared/ -name "*.c" -o -name "*.h" | xargs awk 'length > 120 {print FILENAME":"NR": "length" chars"}' 2>/dev/null | wc -l)
    if [[ $long_lines -gt 0 ]]; then
        warn "Found $long_lines lines > 120 characters"
        ((issues++))
    fi
    
    if [[ $issues -eq 0 ]]; then
        success "C code style checks passed"
    else
        warn "Found $issues style issues (non-blocking)"
    fi
    
    return 0  # Non-blocking
}

# Stage: TypeScript Lint
stage_lint_ts() {
    if [[ -d fuelwise/ui/node_modules ]]; then
        info "Linting TypeScript..."
        cd fuelwise/ui && npm run lint && cd ../..
    else
        warn "Skipping TypeScript lint (no node_modules)"
        warn "Run: cd fuelwise/ui && npm install"
    fi
}

# Stage: Security Check
stage_security() {
    info "Running basic security checks..."
    
    local issues=0
    
    # Check for hardcoded credentials
    if grep -rn "password\s*=" --include="*.c" --include="*.h" --include="*.ts" --include="*.tsx" . 2>/dev/null | grep -v "password_hash" | head -3; then
        warn "Potential hardcoded password found"
        ((issues++))
    fi
    
    # Check for TODO security markers
    if grep -rn "TODO.*security\|FIXME.*security\|XXX.*security" --include="*.c" --include="*.h" . 2>/dev/null | head -3; then
        warn "Found security-related TODO markers"
        ((issues++))
    fi
    
    if [[ $issues -eq 0 ]]; then
        success "No obvious security issues found"
    fi
    
    return 0
}

# Stage: Docker Build
stage_docker() {
    if command -v docker &> /dev/null; then
        info "Building Docker image..."
        docker build -t fuelwise:ci --target api-only .
    else
        warn "Docker not available, skipping"
    fi
}

# Summary
show_summary() {
    echo ""
    echo "=========================================="
    echo "CI Pipeline Summary"
    echo "=========================================="
    echo -e "Passed:  ${GREEN}$STAGE_PASSED${NC}"
    echo -e "Failed:  ${RED}$STAGE_FAILED${NC}"
    echo -e "Skipped: ${YELLOW}$STAGE_SKIPPED${NC}"
    echo ""
    
    if [[ $STAGE_FAILED -gt 0 ]]; then
        error "Pipeline failed with $STAGE_FAILED failures"
    else
        success "Pipeline completed successfully"
    fi
}

# Main
main() {
    echo "FuelWise CI Pipeline"
    echo "===================="
    echo "Started: $(date)"
    echo ""
    
    case "${1:-all}" in
        clean)
            run_stage "Clean" stage_clean
            ;;
        build)
            run_stage "Build Libraries" stage_build
            run_stage "Build APIs" stage_build_api
            ;;
        test)
            run_stage "Unit Tests" stage_test
            run_stage "API Tests" stage_test_api
            ;;
        lint)
            run_stage "Check Generated" stage_check_generated
            run_stage "C Lint" stage_lint_c
            run_stage "TypeScript Lint" stage_lint_ts
            ;;
        security)
            run_stage "Security Check" stage_security
            ;;
        docker)
            run_stage "Docker Build" stage_docker
            ;;
        all)
            run_stage "Clean" stage_clean
            run_stage "Build Libraries" stage_build
            run_stage "Build APIs" stage_build_api
            run_stage "Unit Tests" stage_test
            run_stage "Check Generated" stage_check_generated
            run_stage "C Lint" stage_lint_c
            run_stage "Security Check" stage_security
            ;;
        quick)
            # Quick check for pre-commit
            run_stage "Build Libraries" stage_build
            run_stage "Unit Tests" stage_test
            ;;
        *)
            echo "Usage: $0 [clean|build|test|lint|security|docker|all|quick]"
            exit 1
            ;;
    esac
    
    show_summary
}

main "$@"
