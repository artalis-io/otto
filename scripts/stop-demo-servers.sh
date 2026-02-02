#!/bin/bash
# Stop all demo servers (Carta, Velo, Locus, ClayShards demo)

echo "Stopping demo servers..."

# Kill Locus geocoding server (port 8083)
pkill -f "locus-geocoder" 2>/dev/null && echo "  Stopped: Locus (8083)" || echo "  Not running: Locus"

# Kill Velo route server (port 8082)
pkill -f "velo-server" 2>/dev/null && echo "  Stopped: Velo (8082)" || echo "  Not running: Velo"

# Kill Carta tile server (port 8081)
pkill -f "carta-server" 2>/dev/null && echo "  Stopped: Carta (8081)" || echo "  Not running: Carta"

# Kill Python HTTP server for demo (port 8000)
pkill -f "python.*http.server.*8000" 2>/dev/null && echo "  Stopped: Demo (8000)" || echo "  Not running: Demo"

echo "Done."
