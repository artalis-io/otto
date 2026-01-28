#!/bin/bash
#
# Download OSM PBF data from Geofabrik
#
# Usage:
#   ./scripts/download-osm.sh <region>
#   ./scripts/download-osm.sh hungary
#   ./scripts/download-osm.sh monaco
#   ./scripts/download-osm.sh list
#
# Popular regions:
#   europe/hungary, europe/monaco, europe/austria
#   north-america/us/california, asia/japan
#

set -e

GEOFABRIK_BASE="https://download.geofabrik.de"
DATA_DIR="${DATA_DIR:-./data}"

# Common region mappings (short name -> full path)
declare -A REGIONS=(
    ["hungary"]="europe/hungary"
    ["monaco"]="europe/monaco"
    ["austria"]="europe/austria"
    ["germany"]="europe/germany"
    ["france"]="europe/france"
    ["switzerland"]="europe/switzerland"
    ["netherlands"]="europe/netherlands"
    ["belgium"]="europe/belgium"
    ["poland"]="europe/poland"
    ["czech"]="europe/czech-republic"
    ["slovakia"]="europe/slovakia"
    ["romania"]="europe/romania"
    ["croatia"]="europe/croatia"
    ["serbia"]="europe/serbia"
    ["slovenia"]="europe/slovenia"
    ["california"]="north-america/us/california"
    ["texas"]="north-america/us/texas"
    ["newyork"]="north-america/us/new-york"
    ["japan"]="asia/japan"
    ["australia"]="australia-oceania/australia"
)

usage() {
    echo "Usage: $0 <region>"
    echo ""
    echo "Downloads OSM PBF data from Geofabrik to $DATA_DIR"
    echo ""
    echo "Short names:"
    for key in "${!REGIONS[@]}"; do
        echo "  $key -> ${REGIONS[$key]}"
    done | sort
    echo ""
    echo "Or use full path: europe/hungary, north-america/us/california, etc."
    echo ""
    echo "Examples:"
    echo "  $0 hungary           # Download Hungary"
    echo "  $0 monaco            # Download Monaco (small, good for testing)"
    echo "  $0 europe/austria    # Download Austria (full path)"
    echo "  $0 list              # List available regions"
}

list_regions() {
    echo "Fetching region list from Geofabrik..."
    echo ""
    echo "Available short names:"
    for key in "${!REGIONS[@]}"; do
        echo "  $key"
    done | sort
    echo ""
    echo "For more regions, visit: https://download.geofabrik.de/"
}

download_region() {
    local region="$1"

    # Check if it's a short name
    if [[ -n "${REGIONS[$region]}" ]]; then
        region="${REGIONS[$region]}"
    fi

    # Extract filename from region path
    local filename=$(basename "$region")-latest.osm.pbf
    local url="${GEOFABRIK_BASE}/${region}-latest.osm.pbf"
    local output="${DATA_DIR}/${filename}"

    echo "Region: $region"
    echo "URL: $url"
    echo "Output: $output"
    echo ""

    # Create data directory
    mkdir -p "$DATA_DIR"

    # Download with progress
    echo "Downloading..."
    if command -v curl &> /dev/null; then
        curl -L --progress-bar -o "$output" "$url"
    elif command -v wget &> /dev/null; then
        wget --show-progress -O "$output" "$url"
    else
        echo "Error: curl or wget required"
        exit 1
    fi

    # Show file info
    local size=$(ls -lh "$output" | awk '{print $5}')
    echo ""
    echo "Downloaded: $output ($size)"
    echo ""
    echo "Usage:"
    echo "  ./carta/api/carta-tile-server $output"
    echo "  ./velo/api/velo-route-server $output"
}

# Main
if [[ $# -eq 0 ]]; then
    usage
    exit 1
fi

case "$1" in
    -h|--help|help)
        usage
        ;;
    list)
        list_regions
        ;;
    *)
        download_region "$1"
        ;;
esac
