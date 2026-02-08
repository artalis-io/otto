#!/bin/bash
# Stop all demo servers (Carta, Velo, Locus, demo HTTP server)
#
# Features:
# - Finds servers by binary name (handles any port)
# - Graceful shutdown (SIGTERM) with fallback to SIGKILL
# - Reports PIDs and ports that were freed
# - Cleans up generated config file

cd "$(dirname "$0")/.."

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "Stopping demo servers..."

# Track if anything was stopped
STOPPED_ANY=0

# Stop a process by name pattern with graceful shutdown
# Usage: stop_server "display_name" "pkill_pattern" [timeout_seconds]
stop_server() {
    local name="$1"
    local pattern="$2"
    local timeout="${3:-3}"

    # Find matching PIDs
    local pids=$(pgrep -f "$pattern" 2>/dev/null || true)

    if [ -z "$pids" ]; then
        echo -e "  ${YELLOW}Not running:${NC} $name"
        return 0
    fi

    # Get port info before killing (for display)
    local port_info=""
    for pid in $pids; do
        # Try to get listening port (works on macOS and Linux)
        local port=$(lsof -Pan -p "$pid" -i 2>/dev/null | grep LISTEN | awk '{print $9}' | sed 's/.*://' | head -1)
        if [ -n "$port" ]; then
            port_info="$port_info:$port"
        fi
    done
    port_info=$(echo "$port_info" | sed 's/^://')

    # Try graceful shutdown first (SIGTERM)
    pkill -TERM -f "$pattern" 2>/dev/null || true

    # Wait for processes to exit
    local waited=0
    while [ $waited -lt $timeout ]; do
        if ! pgrep -f "$pattern" >/dev/null 2>&1; then
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done

    # Force kill if still running
    if pgrep -f "$pattern" >/dev/null 2>&1; then
        pkill -KILL -f "$pattern" 2>/dev/null || true
        sleep 0.5
    fi

    # Verify stopped
    if pgrep -f "$pattern" >/dev/null 2>&1; then
        echo -e "  ${RED}Failed to stop:${NC} $name (PIDs: $pids)"
        return 1
    else
        local port_display=""
        if [ -n "$port_info" ]; then
            port_display=" (port $port_info)"
        fi
        echo -e "  ${GREEN}Stopped:${NC} $name$port_display"
        STOPPED_ANY=1
        return 0
    fi
}

# Stop each server type
# Note: patterns match the actual binary names from start-demo-servers.sh
stop_server "Carta tile server" "carta-tile-server"
stop_server "Velo route server" "velo-route-server"
stop_server "Locus geocoder" "locus-geocoder"

# Python HTTP server - match http.server module (any port)
stop_server "Demo HTTP server" "python.*http\.server"

# Clean up generated config file
if [ -f "demo-config.json" ]; then
    rm -f "demo-config.json"
    echo -e "  ${GREEN}Removed:${NC} demo-config.json"
fi

echo ""
if [ $STOPPED_ANY -eq 1 ]; then
    echo -e "${GREEN}All demo servers stopped.${NC}"
else
    echo -e "${YELLOW}No demo servers were running.${NC}"
fi
