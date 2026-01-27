#!/bin/bash
#
# Download OSM PBF map files from Geofabrik
#
# Usage:
#   ./scripts/download-map.sh hungary        # Downloads hungary-latest.osm.pbf
#   ./scripts/download-map.sh germany        # Downloads germany-latest.osm.pbf
#   ./scripts/download-map.sh europe/france  # Downloads france-latest.osm.pbf
#
# See https://download.geofabrik.de/ for available regions

set -e

REGION="${1:-hungary}"
OUTDIR="${2:-.}"

# Normalize region path
case "$REGION" in
    hungary|austria|czechia|slovakia|romania|serbia|croatia|slovenia)
        URL="https://download.geofabrik.de/europe/${REGION}-latest.osm.pbf"
        FILENAME="${REGION}-latest.osm.pbf"
        ;;
    germany|france|spain|italy|poland|netherlands|belgium|switzerland)
        URL="https://download.geofabrik.de/europe/${REGION}-latest.osm.pbf"
        FILENAME="${REGION}-latest.osm.pbf"
        ;;
    */*)
        # Full path like europe/france or north-america/us/california
        BASENAME=$(basename "$REGION")
        URL="https://download.geofabrik.de/${REGION}-latest.osm.pbf"
        FILENAME="${BASENAME}-latest.osm.pbf"
        ;;
    *)
        # Assume Europe
        URL="https://download.geofabrik.de/europe/${REGION}-latest.osm.pbf"
        FILENAME="${REGION}-latest.osm.pbf"
        ;;
esac

OUTPUT="${OUTDIR}/${FILENAME}"

echo "Downloading: $URL"
echo "Output: $OUTPUT"
echo

if command -v curl &> /dev/null; then
    curl -L -o "$OUTPUT" "$URL"
elif command -v wget &> /dev/null; then
    wget -O "$OUTPUT" "$URL"
else
    echo "Error: Neither curl nor wget found. Please install one of them."
    exit 1
fi

echo
echo "Downloaded: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
