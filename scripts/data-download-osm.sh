#!/bin/bash
#
# Download OSM PBF data from Geofabrik
#
# Usage:
#   ./scripts/data-download-osm.sh <region>
#   ./scripts/data-download-osm.sh hungary
#   ./scripts/data-download-osm.sh monaco
#   ./scripts/data-download-osm.sh list
#
# Popular regions:
#   europe/hungary, europe/monaco, europe/austria
#   north-america/us/california, asia/japan
#

set -e

GEOFABRIK_BASE="https://download.geofabrik.de"

# Fallback, used only when Geofabrik is unreachable. OSM France mirrors part of
# the same region tree under /extracts -- monaco is there, hungary is not -- so
# a 404 here means "no mirror for this region", not a broken download.
MIRROR_BASE="https://download.openstreetmap.fr/extracts"

DATA_DIR="${DATA_DIR:-./data}"

# Common region mappings (short name -> full path)
# Format: short_name|full_path|approximate_size
REGION_DATA="monaco|europe/monaco|1 MB
liechtenstein|europe/liechtenstein|2 MB
andorra|europe/andorra|3 MB
luxembourg|europe/luxembourg|40 MB
slovenia|europe/slovenia|100 MB
croatia|europe/croatia|180 MB
slovakia|europe/slovakia|200 MB
hungary|europe/hungary|350 MB
czech|europe/czech-republic|500 MB
austria|europe/austria|700 MB
switzerland|europe/switzerland|400 MB
belgium|europe/belgium|400 MB
netherlands|europe/netherlands|1.2 GB
romania|europe/romania|600 MB
serbia|europe/serbia|180 MB
poland|europe/poland|1.5 GB
germany|europe/germany|4 GB
france|europe/france|4.5 GB
italy|europe/italy|2.2 GB
spain|europe/spain|1.1 GB
uk|europe/great-britain|1.5 GB
europe|europe|30 GB
california|north-america/us/california|1 GB
texas|north-america/us/texas|500 MB
newyork|north-america/us/new-york|400 MB
florida|north-america/us/florida|350 MB
usa|north-america/us|10 GB
canada|north-america/canada|3 GB
mexico|north-america/mexico|500 MB
japan|asia/japan|2 GB
australia|australia-oceania/australia|1 GB"

# Lookup function (works with bash 3.x)
lookup_region() {
    echo "$REGION_DATA" | while IFS='|' read -r short full size; do
        if [ "$short" = "$1" ]; then
            echo "$full"
            return 0
        fi
    done
}

usage() {
    cat << 'EOF'
Download OSM PBF data from Geofabrik

USAGE:
    ./scripts/data-download-osm.sh <region>
    ./scripts/data-download-osm.sh [OPTIONS]

OPTIONS:
    -h, --help      Show this help message
    list            List all available short names

REGIONS:
    Use a short name (see below) or a full Geofabrik path.

    Test regions (small, fast downloads):
      monaco          1 MB    Good for quick tests
      liechtenstein   2 MB    Tiny country
      andorra         3 MB    Small Pyrenees country
      luxembourg     40 MB    Small but complete

    Europe:
      hungary       350 MB    Central Europe
      austria       700 MB    Alpine region
      switzerland   400 MB    Detailed Alpine data
      germany         4 GB    Large, detailed
      france        4.5 GB    Large country
      italy         2.2 GB    Boot-shaped
      spain         1.1 GB    Iberian peninsula
      uk            1.5 GB    Great Britain
      poland        1.5 GB    Eastern Europe
      netherlands   1.2 GB    Very detailed
      belgium       400 MB    Benelux
      czech         500 MB    Central Europe
      slovakia      200 MB    Central Europe
      romania       600 MB    Eastern Europe
      croatia       180 MB    Adriatic coast
      serbia        180 MB    Balkans
      slovenia      100 MB    Small Alpine
      europe         30 GB    Entire continent

    North America:
      california      1 GB    US West Coast
      texas         500 MB    US South
      newyork       400 MB    US Northeast
      florida       350 MB    US Southeast
      usa            10 GB    Entire US
      canada          3 GB    Entire Canada
      mexico        500 MB    Entire Mexico

    Other:
      japan           2 GB    East Asia
      australia       1 GB    Oceania

    Full paths work too:
      europe/hungary
      north-america/us/california
      asia/japan

EXAMPLES:
    ./scripts/data-download-osm.sh monaco           # Quick test (1 MB)
    ./scripts/data-download-osm.sh hungary          # Medium size (350 MB)
    ./scripts/data-download-osm.sh europe/austria   # Full path syntax
    ./scripts/data-download-osm.sh list             # Show all short names

ENVIRONMENT:
    DATA_DIR    Output directory (default: ./data)

MORE REGIONS:
    Visit https://download.geofabrik.de/ for the complete list.
EOF
}

list_regions() {
    echo "Available short names:"
    echo ""
    printf "  %-15s %-8s %s\n" "NAME" "SIZE" "PATH"
    printf "  %-15s %-8s %s\n" "----" "----" "----"
    echo "$REGION_DATA" | while IFS='|' read -r short full size; do
        [ -z "$short" ] && continue
        printf "  %-15s %-8s %s\n" "$short" "$size" "$full"
    done
    echo ""
    echo "For more regions, visit: https://download.geofabrik.de/"
}

# Fetch $1 into $2, failing on HTTP errors rather than saving the error body.
#
# This used to be a bare `curl -L -o`, which exits 0 on a 4xx/5xx and writes the
# response body to the output path. During a Geofabrik outage that put a 3 KB
# Squid error page into data/monaco-latest.osm.pbf, and because the Makefile
# target has no prerequisites, make then considered the file up to date forever.
# Every downstream tool reported a corrupt PBF instead of a failed download.
fetch() {
    if command -v curl > /dev/null 2>&1; then
        curl -fL --retry 3 --retry-delay 2 --progress-bar -o "$2" "$1"
    elif command -v wget > /dev/null 2>&1; then
        wget --tries=3 --show-progress -O "$2" "$1"
    else
        echo "Error: curl or wget required" >&2
        return 1
    fi
}

# An OSM PBF opens with a BlobHeader whose type string is "OSMHeader", within
# the first few dozen bytes. Cheap way to tell a real extract from an HTML
# error page that arrived with a 200.
is_pbf() {
    [ -s "$1" ] && head -c 64 "$1" | grep -qa "OSMHeader"
}

download_region() {
    local region="$1"

    # Check if it's a short name
    local full_path
    full_path=$(lookup_region "$region")
    if [ -n "$full_path" ]; then
        region="$full_path"
    fi

    # Extract filename from region path
    local filename=$(basename "$region")-latest.osm.pbf
    local url="${GEOFABRIK_BASE}/${region}-latest.osm.pbf"
    local mirror_url="${MIRROR_BASE}/${region}-latest.osm.pbf"
    local output="${DATA_DIR}/${filename}"
    local tmp="${output}.part"

    echo "Region: $region"
    echo "URL: $url"
    echo "Output: $output"
    echo ""

    # Create data directory
    mkdir -p "$DATA_DIR"

    # Download to a temporary path and only move it into place once it looks
    # like a real PBF, so a failed download can never leave a file that make
    # will treat as a finished one.
    rm -f "$tmp"
    trap 'rm -f "$tmp"' RETURN

    local source_used=""
    for candidate in "$url" "$mirror_url"; do
        echo "Downloading from $candidate"
        if fetch "$candidate" "$tmp" && is_pbf "$tmp"; then
            source_used="$candidate"
            break
        fi
        echo "  no usable PBF from this source, trying the next one" >&2
        rm -f "$tmp"
    done

    if [ -z "$source_used" ]; then
        echo "" >&2
        echo "Error: could not download a valid PBF for '$region'." >&2
        echo "  tried: $url" >&2
        echo "         $mirror_url" >&2
        echo "" >&2
        echo "Geofabrik may be down; the mirror only carries some regions." >&2
        return 1
    fi

    mv "$tmp" "$output"
    [ "$source_used" = "$url" ] || echo "(fell back to $MIRROR_BASE)"

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
