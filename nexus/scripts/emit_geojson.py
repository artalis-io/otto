#!/usr/bin/env python3
"""Emit a geocoded records JSON as a GeoJSON FeatureCollection styled by geo_tier.

Dataset-agnostic: reads any file produced by geocode_verify.py (records with
lat/lon/geo_tier/...). Uses the simplestyle-spec (marker-color/-size/-symbol) so
it renders directly in Carta, geojson.io, Leaflet, etc. Points are [lon, lat] per
RFC 7946.

  emit_geojson.py <input.geocoded.json> [-o output.geojson]
"""
import json, os, sys, argparse

TIER_STYLE = {
    "GREEN":  ("#2ca02c", "small",  "house"),      # house-level, cross-confirmed
    "YELLOW": ("#ffd700", "small",  "circle"),      # centroid/partial, review
    "APPROX": ("#ff7f0e", "medium", "star"),        # town-level fallback (postcode/trip)
    "RED":    ("#d62728", "large",  "danger"),       # no usable location
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help="geocoded records JSON (from geocode_verify.py)")
    ap.add_argument("-o", "--output", help="output .geojson (default: alongside input)")
    a = ap.parse_args()
    SRC = a.input
    OUT = a.output or os.path.splitext(SRC)[0].replace(".geocoded", "") + ".geojson"
    recs = json.load(open(SRC))["records"]
    feats = []
    skipped = 0
    for o in recs:
        if o.get("lat") is None or o.get("lon") is None:
            skipped += 1; continue
        tier = o.get("geo_tier", "RED")
        color, size, symbol = TIER_STYLE.get(tier, TIER_STYLE["RED"])
        props = {
            "order_no": o.get("order_no"), "customer": o.get("customer"),
            "city": o.get("city"), "zip": o.get("zip"),
            "address": o.get("address_geocode"),
            "weight_kg": o.get("weight_kg"), "pallets": o.get("pallets"),
            "geo_tier": tier, "geo_source": o.get("geo_source"),
            "geo_type": o.get("geo_type"), "geo_cc": o.get("geo_cc"),
            "uncertainty_m": o.get("uncertainty_m"),
            "flags": ",".join(o.get("flags", [])),
            # simplestyle-spec
            "marker-color": color, "marker-size": size, "marker-symbol": symbol,
            "title": f"{o.get('order_no')} · {o.get('city')} · {tier}",
        }
        feats.append({
            "type": "Feature",
            "geometry": {"type": "Point", "coordinates": [o["lon"], o["lat"]]},
            "properties": props,
        })
    fc = {"type": "FeatureCollection",
          "features": sorted(feats, key=lambda f: f["properties"]["geo_tier"])}
    json.dump(fc, open(OUT, "w"), ensure_ascii=False, indent=1)
    from collections import Counter
    tiers = Counter(f["properties"]["geo_tier"] for f in feats)
    print(f"wrote {OUT}: {len(feats)} features ({dict(tiers)}), {skipped} without coords")

if __name__ == "__main__":
    sys.exit(main())
