#!/usr/bin/env sh
# VRP vendor driver TEMPLATE. Copy into ~/artalis.io/data/<vendor>/ and set the
# five placeholders below. Wires the generic repo engines to this vendor's
# schemas + raw exports. Client data never lives in the repo.
set -eu

# ---- per-vendor settings -------------------------------------------------
VENDOR="TEMPLATE"                         # short name (used only in messages)
COUNTRY="XX"                              # ISO code of stops to KEEP in the VRP
DEPOT_KEY="DEPOT"                         # a stable id for the depot row (index 0)
DEPOT_ADDR="Depot address, City, ZIP"     # geocoded once (or set DEPOT_LAT/LON directly)
GRAPH="$HOME/artalis.io/src/otto/data/index/REGION-velo.vlg"   # Velo graph covering the region
PROFILES="truck,car"                     # routing profiles to emit
WEIGHTS="duration,distance"              # fastest (time) and/or shortest (km)
# --------------------------------------------------------------------------

OTTO="${OTTO:-$HOME/artalis.io/src/otto}"
NX="$OTTO/nexus"
D="$(cd "$(dirname "$0")" && pwd)"
export GEOCODE_OFFLINE="${GEOCODE_OFFLINE:-0}"   # set 1 once the cache is warm

echo "== 0. build tools =="
[ -x "$NX/nx_pipeline" ] || make -C "$NX" tools >/dev/null
[ -x "$OTTO/velo/matrix_build" ] || make -C "$OTTO/velo" tools >/dev/null

echo "== 1. ingest orders + vehicles (transform + reconcile gate + semantic check) =="
sh "$NX/scripts/ingest.sh" "$D/raw/orders.csv"   "$D/schemas/orders.schema.json"   "$D/input/orders.json"
sh "$NX/scripts/ingest.sh" "$D/raw/vehicles.csv" "$D/schemas/vehicles.schema.json" "$D/input/vehicles.json"

echo "== 2. geocode + verify (tiered) =="
python3 "$NX/scripts/geocode_verify.py" --orders "$D/input/orders.json"

echo "== 3. build travel matrices (depot + $COUNTRY stops) =="
# Resolve the depot to lat/lon. Prefer an explicit DEPOT_LAT/DEPOT_LON; otherwise
# pull it from the geocode cache (geocode DEPOT_ADDR once with geocode_verify).
if [ -n "${DEPOT_LAT:-}" ] && [ -n "${DEPOT_LON:-}" ]; then
  DLAT="$DEPOT_LAT"; DLON="$DEPOT_LON"
else
  DEPOT_LL=$(python3 -c "import json,sys;c=json.load(open('$D/.geocode_cache/google.json'));e=c['$DEPOT_ADDR'];print(e['lat'],e['lon'])")
  DLAT=${DEPOT_LL% *}; DLON=${DEPOT_LL#* }
fi
python3 "$NX/scripts/build_matrix.py" \
    --orders "$D/input/orders.geocoded.json" \
    --graph  "$GRAPH" \
    --matrix-tool "$OTTO/velo/matrix_build" \
    --profile "$PROFILES" --weight "$WEIGHTS" --country "$COUNTRY" --tiers GREEN,YELLOW \
    --depot-key "$DEPOT_KEY" --depot-lat "$DLAT" --depot-lon "$DLON" \
    --out-dir "$D/matrices"

echo "== 4. emit VRP CSV bundle (per profile/weight) =="
for M in "$D"/matrices/velo_matrix.*.json; do
  B="$D/bundle/$(basename "$M" .json | sed 's/^velo_matrix\.//')"
  python3 "$NX/scripts/emit_vrp_bundle.py" \
      --matrix "$M" --vehicles "$D/input/vehicles.json" --out-dir "$B"
done

echo "== done ($VENDOR). bundles in $D/bundle/ =="
ls -1 "$D"/bundle/*/ 2>/dev/null | sort -u
