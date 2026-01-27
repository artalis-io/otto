#!/bin/bash
#
# Convert OSM PBF to Velo binary graph format (.vlg)
#
# Usage:
#   ./scripts/convert-map.sh hungary-latest.osm.pbf
#   ./scripts/convert-map.sh hungary-latest.osm.pbf output.vlg
#
# This script builds the converter if needed and runs it.

set -e

PBF_FILE="${1}"
VLG_FILE="${2}"

if [ -z "$PBF_FILE" ]; then
    echo "Usage: $0 <input.osm.pbf> [output.vlg]"
    echo
    echo "Examples:"
    echo "  $0 hungary-latest.osm.pbf                  # Creates hungary-latest.vlg"
    echo "  $0 hungary-latest.osm.pbf hungary.vlg      # Creates hungary.vlg"
    exit 1
fi

if [ ! -f "$PBF_FILE" ]; then
    echo "Error: File not found: $PBF_FILE"
    exit 1
fi

# Default output name: replace .osm.pbf with .vlg
if [ -z "$VLG_FILE" ]; then
    VLG_FILE="${PBF_FILE%.osm.pbf}.vlg"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VELO_DIR="$(dirname "$SCRIPT_DIR")"

# Build bench_pbf if not present (it can convert files)
if [ ! -f "$VELO_DIR/bench_pbf" ]; then
    echo "Building converter..."
    make -C "$VELO_DIR" bench_pbf
fi

echo "Converting: $PBF_FILE -> $VLG_FILE"
echo

# bench_pbf parses PBF and saves to VLG
"$VELO_DIR/bench_pbf" "$PBF_FILE" "$VLG_FILE"

echo
echo "Created: $VLG_FILE ($(du -h "$VLG_FILE" | cut -f1))"
