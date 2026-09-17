#!/usr/bin/env python3
"""Address normalization + parsing via libpostal, with a graceful fallback.

libpostal (statistical, trained on global OSM) is used two ways:
  expand(addr)  -> normalized/expanded string variants (abbrev. expansion,
                   diacritic/case normalization) — good geocoder input.
  parse(addr)   -> structured components {house_number, road, city, postcode,
                   country, ...} — an INDEPENDENT parse we can cross-check
                   against a geocoder's returned components, and to detect
                   POI/warehouse names (no road/house_number parsed).

If the `postal` binding isn't installed, falls back to a light regex normalizer
so the pipeline still runs (HAVE_LIBPOSTAL flag exposes which path is active).
"""
import re

try:
    from postal.expand import expand_address as _expand
    from postal.parser import parse_address as _parse
    HAVE_LIBPOSTAL = True
except Exception:
    HAVE_LIBPOSTAL = False

# fallback abbreviation map (HU + common RO/SK tokens) for when libpostal is absent
_ABBR = {
    r"\bsos\.": "soseaua", r"\bnr\.": "", r"\bu\.": "utca", r"\bkrt\.": "korut",
    r"\bút\b": "ut", r"\btér\b": "ter", r"\bhrsz\.?": "", r"\bdep\.": "",
}

def expand(addr):
    """Return a list of normalized variants (best first)."""
    if not addr:
        return []
    if HAVE_LIBPOSTAL:
        try:
            v = _expand(addr)
            return v or [addr.lower()]
        except Exception:
            pass
    s = addr.lower()
    for pat, rep in _ABBR.items():
        s = re.sub(pat, rep, s)
    s = re.sub(r"\s+", " ", s).strip(" ,")
    return [s]

def parse(addr):
    """Return dict of components. Empty/partial when libpostal absent."""
    if not addr:
        return {}
    if HAVE_LIBPOSTAL:
        try:
            return {label: val for val, label in _parse(addr)}
        except Exception:
            return {}
    # crude fallback: pull a trailing 'zip city' if present
    out = {}
    m = re.search(r"(\d{4,6})\s+([^,]+)$", addr.strip())
    if m:
        out["postcode"], out["city"] = m.group(1), m.group(2).strip().lower()
    return out

def looks_like_poi(addr):
    """True if parse finds no road/house_number -> likely a facility/POI name,
    not a street address (caps achievable geocode precision)."""
    p = parse(addr)
    return not (p.get("road") or p.get("house_number") or p.get("house"))

if __name__ == "__main__":
    import sys
    print("libpostal:", HAVE_LIBPOSTAL)
    for a in sys.argv[1:]:
        print(a)
        print("  expand:", expand(a)[:2])
        print("  parse :", parse(a))
        print("  poi?  :", looks_like_poi(a))
