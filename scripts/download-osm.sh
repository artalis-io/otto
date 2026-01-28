#!/bin/bash
#
# Download OSM PBF files from Geofabrik
#
# Usage:
#   ./download-osm.sh [region]
#
# Regions:
#   hungary (default), austria, germany, france, etc.
#   Use full Geofabrik path for other regions: europe/monaco
#

set -e

DATA_DIR="${DATA_DIR:-../data}"
REGION="${1:-hungary}"

# Geofabrik base URL
BASE_URL="https://download.geofabrik.de"

# Map simple names to full paths
case "$REGION" in
    hungary)
        PATH_PART="europe/hungary"
        ;;
    austria)
        PATH_PART="europe/austria"
        ;;
    germany)
        PATH_PART="europe/germany"
        ;;
    france)
        PATH_PART="europe/france"
        ;;
    italy)
        PATH_PART="europe/italy"
        ;;
    spain)
        PATH_PART="europe/spain"
        ;;
    poland)
        PATH_PART="europe/poland"
        ;;
    czech)
        PATH_PART="europe/czech-republic"
        ;;
    slovakia)
        PATH_PART="europe/slovakia"
        ;;
    romania)
        PATH_PART="europe/romania"
        ;;
    monaco)
        PATH_PART="europe/monaco"
        ;;
    liechtenstein)
        PATH_PART="europe/liechtenstein"
        ;;
    *)
        # Assume it's a full path
        PATH_PART="$REGION"
        ;;
esac

# Extract filename from path
FILENAME=$(basename "$PATH_PART")-latest.osm.pbf
URL="$BASE_URL/$PATH_PART-latest.osm.pbf"

# Create data directory
mkdir -p "$DATA_DIR"

# Download
echo "Downloading $REGION from Geofabrik..."
echo "URL: $URL"
echo "Destination: $DATA_DIR/$FILENAME"
echo

if command -v curl &> /dev/null; then
    curl -L --progress-bar -o "$DATA_DIR/$FILENAME" "$URL"
elif command -v wget &> /dev/null; then
    wget --show-progress -O "$DATA_DIR/$FILENAME" "$URL"
else
    echo "Error: curl or wget required"
    exit 1
fi

# Verify download
if [ -f "$DATA_DIR/$FILENAME" ]; then
    SIZE=$(du -h "$DATA_DIR/$FILENAME" | cut -f1)
    echo
    echo "Download complete: $DATA_DIR/$FILENAME ($SIZE)"
else
    echo "Error: Download failed"
    exit 1
fi
