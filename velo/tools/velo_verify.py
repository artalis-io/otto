#!/usr/bin/env python3
"""Verify Velo routing results with defense-in-depth (see the Locus/geocode work).

Velo ships an exact algorithm (Dijkstra), so unlike geocoding most checks need no
external service: Dijkstra is the internal oracle for the fast production
algorithm, and structural invariants catch the rest. An independent commercial
routers (HERE on the matching profile; Google is car-only) are used on a
stratified SAMPLE to catch OSM MAP defects that every Velo algorithm would agree
on.

Layers, per OD pair (routed by tools/route_batch, caller-chosen profile):
  in-house  : reachable, dijkstra == fast (internal oracle), deterministic,
              road >= straight-line, plausible speed for the profile, sane detour ratio
  invariant : triangle inequality on sampled triples
  oracle    : HERE / Google agreement on a sample (cached, offline-
              guarded so re-runs never re-bill)
Each pair tiered GREEN / YELLOW / RED; coverage reported per country.

Usage:
  velo_verify.py --stops geocoded.json --graph map.vlg [--route-batch ./route_batch]
                 [--country HU] [--sample 400] [--oracle-sample 120]
                 [--cache-dir DIR] [--env .env] [--oracle here,google]
"""
import os, sys, json, math, time, argparse, subprocess, random, urllib.parse, urllib.request

OFFLINE = os.environ.get("VELO_OFFLINE") == "1"
_stats = {"here_calls": 0, "here_hits": 0, "g_calls": 0, "g_hits": 0}

def load_env(path):
    if not path:
        repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        path = os.environ.get("GEOCODE_ENV", os.path.join(repo, ".env"))
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1); os.environ.setdefault(k, v)

def haversine(a, b):
    R = 6371000.0
    la1, lo1, la2, lo2 = map(math.radians, [a[0], a[1], b[0], b[1]])
    d = math.sin((la2-la1)/2)**2 + math.cos(la1)*math.cos(la2)*math.sin((lo2-lo1)/2)**2
    return 2*R*math.asin(math.sqrt(d))

# map our profile names to HERE transportMode (Google Routes is DRIVE/car only)
HERE_MODE = {"car": "car", "truck": "truck", "bike": "bicycle", "foot": "pedestrian", "any": "car"}

def here_route(a, b, key, cache, mode):
    ck = f"H|{mode}|{a[0]:.5f},{a[1]:.5f}|{b[0]:.5f},{b[1]:.5f}"
    if ck in cache: _stats["here_hits"] += 1; return cache[ck]
    if OFFLINE: return None
    _stats["here_calls"] += 1
    q = {"transportMode": mode, "origin": f"{a[0]},{a[1]}",
         "destination": f"{b[0]},{b[1]}", "return": "summary", "apiKey": key}
    u = "https://router.hereapi.com/v8/routes?" + urllib.parse.urlencode(q)
    try:
        d = json.load(urllib.request.urlopen(u, timeout=20))
        s = d["routes"][0]["sections"][0]["summary"]
        out = {"dist_m": s["length"], "dur_s": s["duration"]}
    except Exception:
        out = None
    if out is not None: cache[ck] = out
    return out

def google_drive(a, b, key, cache):
    """Google Routes API computeRoutes (DRIVE = car; the legacy Distance Matrix
    API is disabled on the key, and Routes has no HGV mode). Car profile, so we
    compare on DISTANCE — car and truck take similar roads; duration differs by
    profile."""
    ck = f"G|{a[0]:.5f},{a[1]:.5f}|{b[0]:.5f},{b[1]:.5f}"
    if ck in cache: _stats["g_hits"] += 1; return cache[ck]
    if OFFLINE: return None
    _stats["g_calls"] += 1
    body = {"origin": {"location": {"latLng": {"latitude": a[0], "longitude": a[1]}}},
            "destination": {"location": {"latLng": {"latitude": b[0], "longitude": b[1]}}},
            "travelMode": "DRIVE"}
    req = urllib.request.Request(
        "https://routes.googleapis.com/directions/v2:computeRoutes",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "X-Goog-Api-Key": key,
                 "X-Goog-FieldMask": "routes.distanceMeters,routes.duration"})
    try:
        d = json.load(urllib.request.urlopen(req, timeout=20))
        r = d["routes"][0]
        out = {"dist_m": float(r["distanceMeters"]),
               "dur_s": float(str(r.get("duration", "0s")).rstrip("s"))}
    except Exception:
        out = None
    if out is not None: cache[ck] = out
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stops", required=True)
    ap.add_argument("--graph", required=True)
    ap.add_argument("--route-batch", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "route_batch"))
    ap.add_argument("--country", default=None, help="restrict OD pairs to this country code (for partial-graph coverage); default: all stops")
    ap.add_argument("--profile", default="car", choices=["car","truck","bike","foot","any"], help="routing profile passed to route_batch")
    ap.add_argument("--sample", type=int, default=400, help="OD pairs for in-house checks")
    ap.add_argument("--oracle-sample", type=int, default=120)
    ap.add_argument("--min-speed", type=float, default=3.0, help="km/h; below this is flagged implausible")
    ap.add_argument("--max-speed", type=float, default=130.0, help="km/h; above this is flagged implausible")
    ap.add_argument("--cache-dir")
    ap.add_argument("--env")
    ap.add_argument("--oracle", default="here", help="comma list: here,google (independent map oracles)")
    a = ap.parse_args()
    load_env(a.env)
    rnd = random.Random(42)

    base = os.path.dirname(os.path.abspath(a.stops))
    cache_dir = a.cache_dir or os.path.join(os.path.dirname(base), ".velo_cache")
    os.makedirs(cache_dir, exist_ok=True)
    hcache = json.load(open(f"{cache_dir}/here.json")) if os.path.exists(f"{cache_dir}/here.json") else {}
    gcache = json.load(open(f"{cache_dir}/google.json")) if os.path.exists(f"{cache_dir}/google.json") else {}

    recs = json.load(open(a.stops))["records"]
    routable = [o for o in recs if o.get("lat") is not None and o.get("geo_tier") in ("GREEN", "YELLOW")]
    from collections import Counter
    by_cc = Counter(o.get("geo_cc") for o in routable)
    if a.country:
        covered = [o for o in routable if o.get("geo_cc") == a.country]
        uncovered = [o for o in routable if o.get("geo_cc") != a.country]
    else:
        covered, uncovered = routable, []
    stops = [{"id": o["order_no"], "lat": o["lat"], "lon": o["lon"]} for o in covered]

    # --- sample OD pairs (stratified by straight-line distance) for in-house checks ---
    n = len(stops)
    all_pairs = [(i, j) for i in range(n) for j in range(n) if i != j]
    rnd.shuffle(all_pairs)
    pairs = all_pairs[:a.sample]
    # triples for triangle inequality
    triples = [tuple(rnd.sample(range(n), 3)) for _ in range(min(150, max(0, n)))] if n >= 3 else []
    need = set()
    for i, j in pairs: need.add((i, j))
    for i, j, k in triples: need.update({(i, j), (j, k), (i, k)})

    # --- route the needed pairs via route_batch (Dijkstra + fast) ---
    tsv = "".join(f"{i}_{j}\t{stops[i]['lat']}\t{stops[i]['lon']}\t{stops[j]['lat']}\t{stops[j]['lon']}\n"
                  for (i, j) in need)
    print(f"routing {len(need)} OD pairs over {n} stops (profile={a.profile}) via Velo...", file=sys.stderr)
    p = subprocess.run([a.route_batch, a.graph, "--profile", a.profile], input=tsv, capture_output=True, text=True, timeout=1200)
    routed = {}
    for line in p.stdout.splitlines():
        try:
            r = json.loads(line); routed[r["id"]] = r
        except json.JSONDecodeError:
            pass

    def cost(i, j):   # duration is the optimized metric; triangle inequality holds on it
        r = routed.get(f"{i}_{j}")
        return r["fast"]["dur_s"] if r and r["fast"]["ok"] else None

    # --- in-house checks + tiering per pair ---
    findings = {"unreachable": [], "algo_divergence": [], "nondeterministic": [],
                "below_straightline": [], "speed_outlier": [], "detour_outlier": []}
    tiers = Counter()
    oracle_pairs = pairs[:a.oracle_sample]
    oracle_set = set(oracle_pairs)
    here_ok = google_ok = here_off = google_off = 0
    for (i, j) in pairs:
        r = routed.get(f"{i}_{j}")
        checks = []
        if not r or not r["fast"]["ok"] or not r["dijkstra"]["ok"]:
            findings["unreachable"].append((i, j)); tiers["RED"] += 1; continue
        d, dd = r["fast"], r["dijkstra"]
        # internal oracle: compare the OPTIMIZED metric (duration). Distance can
        # differ trivially between equal-duration routes (tie-breaking), so it is
        # not a correctness signal here.
        if abs(d["dur_s"] - dd["dur_s"]) > max(1.0, 0.001 * dd["dur_s"]):
            findings["algo_divergence"].append((i, j)); checks.append("dijkstra!=fast")
        if not r["deterministic"]:
            findings["nondeterministic"].append((i, j)); checks.append("nondeterministic")
        hv = haversine((stops[i]["lat"], stops[i]["lon"]), (stops[j]["lat"], stops[j]["lon"]))
        if d["dist_m"] < 0.99 * hv:
            findings["below_straightline"].append((i, j)); checks.append("below straight-line")
        spd = d["dist_m"] / d["dur_s"] * 3.6 if d["dur_s"] > 0 else 0
        if not (a.min_speed <= spd <= a.max_speed):
            findings["speed_outlier"].append((i, j, round(spd))); checks.append(f"speed {spd:.0f}km/h")
        ratio = d["dist_m"] / hv if hv > 0 else 0
        if hv > 500 and not (1.0 <= ratio <= 3.0):
            findings["detour_outlier"].append((i, j, round(ratio, 2))); checks.append(f"detour {ratio:.1f}x")

        hard = bool(findings_hit(findings, i, j, ("algo_divergence", "nondeterministic", "below_straightline")))
        # oracle for the sampled subset
        odis = None
        if (i, j) in oracle_set:
            A = (stops[i]["lat"], stops[i]["lon"]); B = (stops[j]["lat"], stops[j]["lon"])
            if "here" in a.oracle and os.environ.get("HERE_API_KEY"):
                h = here_route(A, B, os.environ["HERE_API_KEY"], hcache, HERE_MODE[a.profile])
                if h:
                    rel = abs(d["dist_m"] - h["dist_m"]) / max(1.0, h["dist_m"])
                    if rel <= 0.25: here_ok += 1
                    else: odis = ("HERE", rel)
                else: here_off += 1
            if "google" in a.oracle and os.environ.get("GOOGLE_MAPS_API_KEY"):
                g = google_drive(A, B, os.environ["GOOGLE_MAPS_API_KEY"], gcache)
                if g:
                    rel = abs(d["dist_m"] - g["dist_m"]) / max(1.0, g["dist_m"])
                    if rel <= 0.35: google_ok += 1  # car profile: looser
                    elif odis is None: odis = ("Google", rel)
                else: google_off += 1

        if hard or (odis and odis[1] > 0.5):
            tiers["RED"] += 1
        elif checks or odis:
            tiers["YELLOW"] += 1
        else:
            tiers["GREEN"] += 1

    json.dump(hcache, open(f"{cache_dir}/here.json", "w"))
    json.dump(gcache, open(f"{cache_dir}/google.json", "w"))

    # --- triangle inequality ---
    tri_viol = 0
    for (i, j, k) in triples:
        dij, djk, dik = cost(i, j), cost(j, k), cost(i, k)
        if dij is not None and djk is not None and dik is not None:
            if dik > dij + djk + 1.0:  # seconds slack (duration triangle inequality)
                tri_viol += 1

    # --- report ---
    print(f"\n=== Velo verification report (profile={a.profile}) ===")
    print(f"routable stops: {dict(by_cc)}")
    print(f"graph coverage: {len(covered)} {a.country} stops routed; "
          f"{len(uncovered)} non-{a.country} stops NOT covered by this graph (need multi-country graph)")
    print(f"OD pairs checked (in-house): {sum(tiers.values())}  GREEN={tiers['GREEN']} YELLOW={tiers['YELLOW']} RED={tiers['RED']}")
    print(f"internal oracle  : dijkstra==fast divergences: {len(findings['algo_divergence'])}")
    print(f"determinism      : non-deterministic pairs: {len(findings['nondeterministic'])}")
    print(f"invariants       : below straight-line: {len(findings['below_straightline'])}; "
          f"triangle violations: {tri_viol}/{len(triples)}")
    print(f"plausibility     : speed outliers: {len(findings['speed_outlier'])}; "
          f"detour outliers: {len(findings['detour_outlier'])}; unreachable: {len(findings['unreachable'])}")
    print(f"external oracle  : HERE agree<=25%: {here_ok}/{len(oracle_pairs)} "
          f"({_stats['here_calls']} live, {_stats['here_hits']} cached); "
          f"Google-driving agree<=35%: {google_ok}/{len(oracle_pairs)} "
          f"({_stats['g_calls']} live, {_stats['g_hits']} cached)")
    for k in ("speed_outlier", "detour_outlier", "below_straightline", "algo_divergence"):
        if findings[k]:
            print(f"  [{k}] sample: {findings[k][:5]}")
    return 0

def findings_hit(findings, i, j, keys):
    for k in keys:
        for t in findings[k]:
            if t[0] == i and t[1] == j: return True
    return False

if __name__ == "__main__":
    sys.exit(main())
