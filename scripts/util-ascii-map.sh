#!/bin/bash
#
# util-ascii-map.sh - Generate ASCII art map for a location
#
# Usage:
#   ./scripts/util-ascii-map.sh LAT LON ZOOM [OPTIONS]
#
# Examples:
#   ./scripts/util-ascii-map.sh 47.497 19.040 11              # Budapest
#   ./scripts/util-ascii-map.sh 48.858 2.347 12 -w 60         # Paris
#   ./scripts/util-ascii-map.sh 40.748 -73.985 13 -c braille  # NYC
#
# Options:
#   -w, --width WIDTH    Width per tile (default: 50)
#   -g, --grid NxM       Grid size, e.g. 2x2 (default: 2x2)
#   -c, --charset CHAR   Charset: simple, extended, blocks, braille
#   -o, --output FILE    Output file (default: stdout)
#   -p, --port PORT      Server port (default: 8081)
#   -i, --invert         Invert for light backgrounds
#

set -e

# Defaults
WIDTH=50
GRID_X=2
GRID_Y=2
CHARSET="extended"
OUTPUT=""
PORT=8081
INVERT=0

# Parse arguments
LAT=""
LON=""
ZOOM=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -w|--width)
            WIDTH="$2"; shift 2 ;;
        -g|--grid)
            GRID_X=$(echo "$2" | cut -dx -f1)
            GRID_Y=$(echo "$2" | cut -dx -f2)
            shift 2 ;;
        -c|--charset)
            CHARSET="$2"; shift 2 ;;
        -o|--output)
            OUTPUT="$2"; shift 2 ;;
        -p|--port)
            PORT="$2"; shift 2 ;;
        -i|--invert)
            INVERT=1; shift ;;
        -h|--help)
            head -25 "$0" | tail -23
            exit 0 ;;
        *)
            if [[ -z "$LAT" ]]; then LAT="$1"
            elif [[ -z "$LON" ]]; then LON="$1"
            elif [[ -z "$ZOOM" ]]; then ZOOM="$1"
            fi
            shift ;;
    esac
done

if [[ -z "$LAT" || -z "$LON" || -z "$ZOOM" ]]; then
    echo "Usage: $0 LAT LON ZOOM [OPTIONS]" >&2
    echo "Run with --help for options" >&2
    exit 1
fi

# Calculate center tile coordinates
read TILE_X TILE_Y < <(python3 -c "
import math
lat, lon, zoom = $LAT, $LON, $ZOOM
n = 2 ** zoom
x = int((lon + 180) / 360 * n)
lat_rad = math.radians(lat)
y = int((1 - math.log(math.tan(lat_rad) + 1/math.cos(lat_rad)) / math.pi) / 2 * n)
print(x, y)
")

# Calculate tile range (center the grid on the location)
X_START=$((TILE_X - GRID_X / 2))
X_END=$((X_START + GRID_X - 1))
Y_START=$((TILE_Y - GRID_Y / 2))
Y_END=$((Y_START + GRID_Y - 1))

# Create temp directory
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Build query params
PARAMS="width=$WIDTH&charset=$CHARSET"
if [[ $INVERT -eq 1 ]]; then
    PARAMS="$PARAMS&invert=1"
fi

# Fetch all tiles
echo "Fetching ${GRID_X}x${GRID_Y} tiles at zoom $ZOOM..." >&2
for y in $(seq $Y_START $Y_END); do
    for x in $(seq $X_START $X_END); do
        curl -s "http://localhost:$PORT/tiles/$ZOOM/$x/$y.txt?$PARAMS" > "$TMPDIR/tile_${y}_${x}.txt"
    done
done

# Stitch tiles
echo "Stitching..." >&2
RESULT="$TMPDIR/result.txt"
> "$RESULT"

for y in $(seq $Y_START $Y_END); do
    # Build paste command for this row
    FILES=""
    for x in $(seq $X_START $X_END); do
        tr -d '\r' < "$TMPDIR/tile_${y}_${x}.txt" > "$TMPDIR/tile_${y}_${x}_clean.txt"
        FILES="$FILES $TMPDIR/tile_${y}_${x}_clean.txt"
    done
    paste -d '' $FILES >> "$RESULT"
done

# Output
if [[ -n "$OUTPUT" ]]; then
    cp "$RESULT" "$OUTPUT"
    echo "Saved to $OUTPUT" >&2
    head -1 "$OUTPUT" | wc -c | xargs -I{} echo "Size: {} chars wide x $(wc -l < "$OUTPUT") lines" >&2
else
    cat "$RESULT"
fi
