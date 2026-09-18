#!/usr/bin/env python3
"""
build_matrix.py - Nexus pipeline step: geocoded orders -> Surge travel matrix.

Turns a geocoded-orders JSON (Stage D output) into an all-pairs travel matrix in
Surge's native `travel` shape (sg_set_travel_matrix / JSON API), routed by Velo's
matrix_build tool. Dataset-agnostic: depot, country filter, profiles, tiers and
carried attributes are all arguments -- nothing client-specific is baked in.

Indexing contract (strict 1:1 bijection):
  * index 0 is the depot when --depot-* is given, otherwise index 0 is the first
    order-line;
  * every other index is exactly one order-line, in input file order;
  * NO location de-duplication -- co-located orders keep distinct indices;
  * locations[i].key is unique: the order's id, with a #<occurrence> suffix where
    an id repeats (order_no is NOT unique, so never join on it alone).

Output per profile:
  <out-dir>/velo_matrix.<profile>.json   (meta + travel + locations)
  <out-dir>/velo_matrix.<profile>.csv    (long format)

Usage:
  build_matrix.py --orders geocoded.json --graph map.vlg --matrix-tool ./matrix_build \\
                  --profile truck,car [--country HU] [--tiers GREEN,YELLOW] \\
                  [--depot-key DEPOT --depot-lat 47.6 --depot-lon 18.6] \\
                  [--id-field id] [--attrs id,order_no,...] --out-dir DIR
"""
import argparse, csv, datetime, json, os, subprocess, sys

DEFAULT_ATTRS = ["id", "order_no", "ship_addr_id", "customer", "city", "zip",
                 "lat", "lon", "pallets", "weight_kg", "tw_start", "tw_end",
                 "service_min", "delivery_date"]


def load_stops(orders_path, country, tiers, id_field):
    """Filtered order-lines in FILE ORDER (the sequence matrix_build indexes)."""
    doc = json.load(open(orders_path))
    recs = doc["records"] if isinstance(doc, dict) and "records" in doc else doc
    out = []
    for r in recs:
        if r.get("lat") is None or r.get("lon") is None:
            continue
        if tiers and r.get("geo_tier") not in tiers:
            continue
        if country and r.get("geo_cc") != country:
            continue
        out.append(r)
    return out


def unique_keys(stops, id_field):
    """order id, with a #<occurrence> suffix only where the id repeats."""
    from collections import Counter
    total = Counter(str(r.get(id_field)) for r in stops)
    seen = Counter()
    keys = []
    for r in stops:
        i = str(r.get(id_field))
        seen[i] += 1
        keys.append(f"{i}#{seen[i]}" if total[i] > 1 else i)
    return keys


def run_matrix_tool(tool, graph, profile, weight, stops, depot):
    """Feed 'idx\\tlat\\tlon' to matrix_build; parse S/M lines.
    Returns (snap_node[], snap_off[], dur[][], dist[][]) over N = depot? + stops."""
    locs = ([depot] if depot else []) + [(s["lat"], s["lon"]) for s in stops]
    N = len(locs)
    tsv = "".join(f"{i}\t{la}\t{lo}\n" for i, (la, lo) in enumerate(locs))
    p = subprocess.run([tool, graph, "--profile", profile, "--weight", weight], input=tsv,
                       capture_output=True, text=True, timeout=7200)
    if p.returncode != 0:
        raise RuntimeError(f"matrix_build failed (profile={profile}, weight={weight}): {p.stderr.strip()}")
    snap_node = [None] * N
    snap_off = [None] * N
    dur = [[0.0] * N for _ in range(N)]
    dist = [[0.0] * N for _ in range(N)]
    for ln in p.stdout.splitlines():
        f = ln.split()
        if not f:
            continue
        if f[0] == "S":
            i = int(f[1]); snap_node[i] = int(f[2]); snap_off[i] = float(f[3])
        elif f[0] == "M":
            i, j = int(f[1]), int(f[2]); dur[i][j] = float(f[3]); dist[i][j] = float(f[4])
    return snap_node, snap_off, dur, dist


def build_locations(stops, keys, snap_node, snap_off, depot, depot_key, attrs, id_field):
    """Enriched locations[] with the depot (if any) at index 0. Pure."""
    locs = []
    off = 0
    if depot:
        locs.append({"index": 0, "role": "depot", "key": depot_key,
                     "lat": depot[0], "lon": depot[1],
                     "snap_node": snap_node[0], "snap_offset_m": snap_off[0]})
        off = 1
    for i, r in enumerate(stops):
        loc = {"index": i + off, "role": "stop", "key": keys[i]}
        for a in attrs:
            loc[a] = r.get(a)
        loc["lat"] = r.get("lat"); loc["lon"] = r.get("lon")   # always present for Surge
        loc["snap_node"] = snap_node[i + off]
        loc["snap_offset_m"] = snap_off[i + off]
        locs.append(loc)
    return locs


def assemble(profile, graph, weight, locations, dur, dist, depot_index, generated_at):
    """Assemble the Surge-native doc from enriched locations + matrices. Pure."""
    N = len(locations)
    unreachable = sum(1 for i in range(N) for j in range(N)
                      if i != j and (dur[i][j] < 0 or dist[i][j] < 0))
    durations = [dur[i][j] for i in range(N) for j in range(N)]
    distances = [dist[i][j] for i in range(N) for j in range(N)]
    weight_desc = ("duration (fastest path); distance is length along that path"
                   if weight == "duration" else
                   "distance (shortest path); duration is time along that path")
    doc = {
        "meta": {
            "generated_by": "nexus build_matrix.py + velo matrix_build (one-to-all Dijkstra, profile-faithful)",
            "generated_at_utc": generated_at,
            "graph": graph,
            "profile": profile,
            "weight_optimized": weight_desc,
            "units": {"duration": "seconds", "distance": "meters"},
            "location_count": N,
            "depot_index": depot_index,
            "matrix_order": "row-major; entry[i*N+j] = i -> j; diagonal = 0.0",
            "reachability": "ALL PAIRS ROUTABLE" if unreachable == 0 else f"{unreachable} UNREACHABLE",
            "indexing": "strict 1:1 bijection: matrix index i <-> locations[i].key "
                        "(unique per order-line); NO location de-duplication",
            "join": "join a normalized order via its id (+#occurrence when id repeats); "
                    "order_no is not unique",
        },
        "travel": {"location_count": N, "distances": distances, "durations": durations},
        "locations": locations,
    }
    return doc


def validate_bijection(doc):
    """Raise AssertionError unless index<->key is a strict bijection."""
    N = doc["travel"]["location_count"]
    locs = doc["locations"]
    assert len(locs) == N, f"locations {len(locs)} != N {N}"
    assert [l["index"] for l in locs] == list(range(N)), "index not contiguous 0..N-1"
    keys = [l["key"] for l in locs]
    assert len(set(keys)) == N, "keys not unique -> not a bijection"
    assert len(doc["travel"]["durations"]) == N * N == len(doc["travel"]["distances"]), "matrix size"
    for i in range(N):
        assert doc["travel"]["durations"][i * N + i] == 0.0, f"diagonal dur[{i}] != 0"
        assert doc["travel"]["distances"][i * N + i] == 0.0, f"diagonal dist[{i}] != 0"
    return True


def write_csv(path, locations, dur, dist):
    N = len(locations)
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["from_key", "to_key", "from_idx", "to_idx",
                    "from_order_no", "to_order_no", "duration_s", "distance_m"])
        for i in range(N):
            for j in range(N):
                if i == j:
                    continue
                w.writerow([locations[i]["key"], locations[j]["key"], i, j,
                            locations[i].get("order_no", ""), locations[j].get("order_no", ""),
                            f"{dur[i][j]:.1f}", f"{dist[i][j]:.1f}"])


def main():
    ap = argparse.ArgumentParser(description="Build a Surge travel matrix from geocoded orders")
    ap.add_argument("--orders", required=True)
    ap.add_argument("--graph", required=True)
    ap.add_argument("--matrix-tool", required=True, help="path to velo matrix_build binary")
    ap.add_argument("--profile", default="truck", help="comma list: truck,car,bike,foot,any")
    ap.add_argument("--weight", default="duration", help="comma list: duration (fastest), distance (shortest)")
    ap.add_argument("--country", default=None, help="restrict to this geo_cc (e.g. HU)")
    ap.add_argument("--tiers", default="GREEN,YELLOW", help="accepted geo_tier values")
    ap.add_argument("--depot-key", default=None)
    ap.add_argument("--depot-lat", type=float, default=None)
    ap.add_argument("--depot-lon", type=float, default=None)
    ap.add_argument("--id-field", default="id")
    ap.add_argument("--attrs", default=",".join(DEFAULT_ATTRS))
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--generated-at", default=None, help="override timestamp (for reproducible output)")
    a = ap.parse_args()

    tiers = set(t for t in a.tiers.split(",") if t)
    attrs = [x for x in a.attrs.split(",") if x]
    profiles = [p for p in a.profile.split(",") if p]
    weights = [w for w in a.weight.split(",") if w]
    for w in weights:
        if w not in ("duration", "distance"):
            sys.exit(f"fatal: --weight must be duration or distance, got '{w}'")
    depot = None
    if a.depot_lat is not None and a.depot_lon is not None:
        depot = (a.depot_lat, a.depot_lon)
        if not a.depot_key:
            a.depot_key = "DEPOT"
    generated_at = a.generated_at or (datetime.datetime.now(datetime.timezone.utc)
                                      .replace(microsecond=0, tzinfo=None).isoformat() + "Z")
    os.makedirs(a.out_dir, exist_ok=True)

    stops = load_stops(a.orders, a.country, tiers, a.id_field)
    keys = unique_keys(stops, a.id_field)
    if len(set(keys)) != len(keys):
        sys.exit("fatal: could not derive unique keys from id-field "
                 f"'{a.id_field}' (duplicate ids without distinguishable occurrences)")
    print(f"build_matrix: {len(stops)} order-lines"
          f"{' + depot' if depot else ''} (country={a.country}, tiers={sorted(tiers)})",
          file=sys.stderr)

    for prof in profiles:
        for weight in weights:
            snap_node, snap_off, dur, dist = run_matrix_tool(a.matrix_tool, a.graph, prof, weight, stops, depot)
            locs = build_locations(stops, keys, snap_node, snap_off, depot, a.depot_key, attrs, a.id_field)
            doc = assemble(prof, a.graph, weight, locs, dur, dist, 0 if depot else None, generated_at)
            validate_bijection(doc)
            jpath = os.path.join(a.out_dir, f"velo_matrix.{prof}.{weight}.json")
            cpath = os.path.join(a.out_dir, f"velo_matrix.{prof}.{weight}.csv")
            json.dump(doc, open(jpath, "w"))
            write_csv(cpath, locs, dur, dist)
            N = doc["travel"]["location_count"]
            print(f"  [{prof}/{weight}] N={N} {doc['meta']['reachability']} -> {jpath}, {cpath}", file=sys.stderr)


if __name__ == "__main__":
    main()
