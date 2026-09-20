#!/usr/bin/env python3
"""
build_surge_request.py - Nexus pipeline step: VRP bundle -> Surge solve request.

Turns a bundle (orders.csv, distance.csv, duration.csv, vehicles.csv -- see
emit_vrp_bundle.py) into a Surge POST /api/v1/solve JSON body. The bundle's
location order IS the travel-matrix index order (row 0 = depot, rows 1..N =
stops), so:

  * locations[]  -> one entry per bundle row, indexed 0..N (the travel index)
  * depots[]     -> the depot row(s) (role=depot), location_id = their index (0)
  * tasks[]      -> one delivery task per stop, location_id = its travel index
  * requests[]   -> one delivery-only request per task
  * travel{}     -> location_count + the square distance/duration matrices

Multiple routes per vehicle use Surge's native multi-trip (max_trips /
trip_reload_seconds): each trip departs and returns to the depot, capacity
resets per trip. Fleet: own vehicles plus optional subcontractor clones (high
fixed cost, used only when the own fleet can't cover -- "unlimited subs").

Everything policy-ish is a flag; the bundle alone (no other input) drives the
structure, so this is vendor-neutral.

Usage:
  build_surge_request.py --bundle bundle/truck.distance \\
    --demand-cols pallets,weight_kg --capacity-cols capacity_pallets,capacity_kg \\
    --max-trips 0 --trip-reload-seconds 1800 \\
    --shift 06:00-18:00 --max-duration-min 540 \\
    --sub-col is_subcontractor --sub-clones 5 --own-fixed-cost 100 --sub-fixed-cost 100000 \\
    --objective vehicles-then-distance --max-time-seconds 30 --out request.json
"""
import argparse, csv, json, os, sys


def read_csv(path):
    with open(path) as fh:
        return list(csv.reader(fh))


def to_num(s, default=0.0):
    s = (s or "").strip()
    if s == "":
        return default
    try:
        f = float(s)
        return int(f) if f.is_integer() else f
    except ValueError:
        return default


def hhmm_to_sec(s):
    """'H:MM' / 'HH:MM' -> seconds from midnight; '' -> None."""
    s = (s or "").strip()
    if not s or ":" not in s:
        return None
    h, m = s.split(":")[:2]
    return int(h) * 3600 + int(m) * 60


def flatten_square(rows):
    """rows = labelled square CSV (header + N rows, first col = key). -> (N, flat)."""
    keys = [r[0] for r in rows[1:]]
    N = len(keys)
    flat = []
    for i in range(N):
        cells = rows[i + 1][1:]
        assert len(cells) == N, f"row {i} has {len(cells)} cols, expected {N}"
        flat.extend(to_num(c) for c in cells)
    return N, flat, keys


def parse_shift(s):
    """'HH:MM-HH:MM' -> (early_sec, late_sec)."""
    a, b = s.split("-")
    return hhmm_to_sec(a), hhmm_to_sec(b)


def main():
    ap = argparse.ArgumentParser(description="Build a Surge solve request from a VRP bundle")
    ap.add_argument("--bundle", required=True, help="bundle dir with orders/distance/duration/vehicles.csv")
    ap.add_argument("--demand-cols", default="pallets,weight_kg", help="order columns -> demand dimensions")
    ap.add_argument("--capacity-cols", default="capacity_pallets,capacity_kg", help="vehicle columns -> capacity dims")
    ap.add_argument("--max-trips", type=int, default=0,
                    help="trips per vehicle (0 = unlimited). Surge enforces max_duration "
                         "per shift, so unlimited is bounded by the working day.")
    ap.add_argument("--trip-reload-seconds", type=int, default=1800)
    ap.add_argument("--shift", default="06:00-18:00", help="HH:MM-HH:MM depot/vehicle shift window")
    ap.add_argument("--max-duration-min", type=int, default=540, help="max on-duty minutes per vehicle")
    ap.add_argument("--sub-col", default="is_subcontractor", help="vehicle column flagging subcontractors")
    ap.add_argument("--sub-clones", type=int, default=5, help="times to replicate each subcontractor (unlimited-ish)")
    ap.add_argument("--plate-col", default="plate",
                    help="vehicle column holding the plate; the part before '+' is the physical base plate")
    ap.add_argument("--task-ref-col", default="order_no",
                    help="order column holding the human order id, carried onto each task as 'ref' "
                         "(surfaced in the map/plan; falls back to task id when the column is absent)")
    ap.add_argument("--dedupe-vehicle-configs", action=argparse.BooleanOptionalAction, default=True,
                    help="collapse own vehicles that share a base plate (alternative configs of one "
                         "physical truck, e.g. rigid-solo vs rigid+drawbar) to the highest-capacity "
                         "config, so a truck can't be used in two configs at once")
    ap.add_argument("--own-fixed-cost", type=float, default=100.0)
    ap.add_argument("--sub-fixed-cost", type=float, default=100000.0)
    ap.add_argument("--objective", default="vehicles-then-distance",
                    choices=["vehicles-then-distance", "distance", "duration"])
    ap.add_argument("--hard-max-duration", action=argparse.BooleanOptionalAction, default=True,
                    help="treat vehicle max_duration as a hard (legal HoS) limit, not a penalty")
    ap.add_argument("--hard-capacity", action=argparse.BooleanOptionalAction, default=True,
                    help="treat vehicle capacity as a hard constraint, not a penalty")
    ap.add_argument("--hard-time-windows", action=argparse.BooleanOptionalAction, default=False,
                    help="treat delivery time windows as hard (reject late insertions), not a "
                         "time-warp penalty. Off by default (a feasible solve already respects "
                         "windows; this also guarantees it during search)")
    ap.add_argument("--max-iterations", type=int, default=20000)
    ap.add_argument("--demand-sign", type=int, default=1,
                    help="0=pickup+/delivery- (Surge default), 1=pickup-/delivery+ (matches positive delivery demand)")
    ap.add_argument("--max-time-seconds", type=int, default=30)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--unassigned-penalty", type=float, default=1e6)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    orders = read_csv(os.path.join(a.bundle, "orders.csv"))
    ohdr = orders[0]; oidx = {c: ohdr.index(c) for c in ohdr}
    orows = orders[1:]
    dN, dflat, dkeys = flatten_square(read_csv(os.path.join(a.bundle, "distance.csv")))
    uN, uflat, ukeys = flatten_square(read_csv(os.path.join(a.bundle, "duration.csv")))
    N = len(orows)
    assert dN == uN == N, f"matrix/orders size mismatch: dist {dN}, dur {uN}, orders {N}"
    assert [r[oidx["key"]] for r in orows] == dkeys == ukeys, "keys not aligned across bundle files"

    vrows = read_csv(os.path.join(a.bundle, "vehicles.csv"))
    req, n_own, n_sub, collapsed = assemble_request(orows, oidx, dflat, uflat, N, vrows, a)
    json.dump(req, open(a.out, "w"))
    if collapsed:
        print(f"build_surge_request: collapsed {len(collapsed)} duplicate physical vehicle(s) "
              f"(shared base plate, kept highest-capacity config):", file=sys.stderr)
        for bp, dropped, kept in collapsed:
            print(f"    {bp}: dropped cap {dropped} (kept {kept})", file=sys.stderr)
    print(f"build_surge_request: N={N} locations ({len(req['depots'])} depot, {len(req['tasks'])} deliveries), "
          f"{len(req['vehicles'])} vehicles ({n_own} own + {n_sub} sub-clones), dim={req['dimension_count']}, "
          f"max_trips={a.max_trips}, objective={a.objective} -> {a.out}", file=sys.stderr)


def assemble_request(orows, oidx, dflat, uflat, N, vrows, a):
    """Pure core: parsed bundle rows + options -> Surge request dict. Testable."""
    shift_early, shift_late = parse_shift(a.shift)
    max_dur = a.max_duration_min * 60
    demand_cols = [c for c in a.demand_cols.split(",") if c]
    cap_cols = [c for c in a.capacity_cols.split(",") if c]
    assert len(demand_cols) == len(cap_cols), "demand and capacity must have same dimension count"
    dim = len(demand_cols)

    # locations: index == travel-matrix index == bundle row order
    locations = [{"x": to_num(r[oidx["lon"]]), "y": to_num(r[oidx["lat"]])} for r in orows]

    # depots (role == depot); location_id == their travel index
    depots = []
    for i, r in enumerate(orows):
        if r[oidx["role"]] == "depot":
            depots.append({"id": len(depots), "location_id": i,
                           "tw_early": shift_early, "tw_late": shift_late})
    if not depots:
        raise ValueError("no depot row (role=depot) in orders.csv")

    # tasks + requests: one delivery per stop, location_id == its travel index
    tasks, requests = [], []
    for i, r in enumerate(orows):
        if r[oidx["role"]] == "depot":
            continue
        tid = len(tasks)
        tw_e = hhmm_to_sec(r[oidx["tw_start"]]) if "tw_start" in oidx else None
        tw_l = hhmm_to_sec(r[oidx["tw_end"]]) if "tw_end" in oidx else None
        task = {
            "id": tid, "type": "delivery", "location_id": i,
            "tw_early": shift_early if tw_e is None else tw_e,
            "tw_late": shift_late if tw_l is None else tw_l,
            "service_seconds": int(to_num(r[oidx["service_min"]]) * 60) if "service_min" in oidx else 0,
            "demand": [to_num(r[oidx[c]]) if c in oidx else 0.0 for c in demand_cols],
        }
        task_ref_col = getattr(a, "task_ref_col", "order_no")
        if task_ref_col in oidx and str(r[oidx[task_ref_col]]).strip():
            task["ref"] = str(r[oidx[task_ref_col]]).strip()   # human order id for the map/plan
        tasks.append(task)
        requests.append({"id": tid, "delivery_task_id": tid,
                         "unassigned_penalty": a.unassigned_penalty})

    # vehicles: own as-is + subcontractor clones (high fixed cost -> used only if needed)
    vi = {c: vrows[0].index(c) for c in vrows[0]}
    def is_sub(row):
        return a.sub_col in vi and str(row[vi[a.sub_col]]).strip().lower() in ("true", "1", "yes")
    def cap_of(row):
        return [to_num(row[vi[c]]) if c in vi else 0.0 for c in cap_cols]
    vehicles, n_own, n_sub = [], 0, 0
    def add_vehicle(cap, fixed, ref=None):
        v = {
            "id": len(vehicles), "start_depot_id": 0, "end_depot_id": 0,
            "capacity": cap, "shift_early": shift_early, "shift_late": shift_late,
            "max_duration": max_dur,
            "max_trips": a.max_trips, "trip_reload_seconds": a.trip_reload_seconds,
            "fixed_cost": fixed,
            "cost_per_distance": 1.0 if a.objective != "duration" else 0.0,
            "cost_per_duration": 1.0 if a.objective == "duration" else 0.0,
        }
        if ref:
            v["ref"] = ref   # base plate, for the map/plan legend
        vehicles.append(v)
    own_rows = [r for r in vrows[1:] if not is_sub(r)]
    sub_rows = [r for r in vrows[1:] if is_sub(r)]

    # Collapse alternative configs of the same PHYSICAL vehicle. A drawbar truck
    # appears twice in the fleet: once as a solo rigid and once as the rigid+trailer
    # combo, sharing a base plate ("SLZ-098" vs "SLZ-098+WFB-869"). They are
    # alternatives for one truck, so at most one may run at a time. Surge has no
    # vehicle-level "use at most one" constraint, but the higher-capacity config
    # dominates (same depot/shift/duration/cost/travel), so keeping only the
    # max-capacity config per base plate is equivalent and keeps each truck a
    # single asset. collapsed: list of (base_plate, dropped_cap, kept_cap).
    collapsed = []
    getattr_ok = getattr(a, "dedupe_vehicle_configs", True)
    plate_col = getattr(a, "plate_col", "plate")
    if getattr_ok and plate_col in vi:
        def base_plate(r):
            return str(r[vi[plate_col]]).split("+")[0].strip()
        groups = {}
        for r in own_rows:
            groups.setdefault(base_plate(r), []).append(r)
        kept = []
        for bp, grp in groups.items():
            best = max(grp, key=lambda r: sum(cap_of(r)))
            kept.append(best)
            for r in grp:
                if r is not best:
                    collapsed.append((bp, cap_of(r), cap_of(best)))
        # preserve original file order of kept rows
        keep_set = {id(r) for r in kept}
        own_rows = [r for r in own_rows if id(r) in keep_set]

    def plate_of(row):
        return str(row[vi[plate_col]]).split("+")[0].strip() if plate_col in vi else None
    for row in own_rows:
        add_vehicle(cap_of(row), a.own_fixed_cost, plate_of(row)); n_own += 1
    for row in sub_rows:
        for c in range(max(1, a.sub_clones)):
            ref = f"{plate_of(row)}#{c + 1}" if plate_of(row) else None
            add_vehicle(cap_of(row), a.sub_fixed_cost, ref); n_sub += 1

    req = {
        "config": {"max_iterations": a.max_iterations, "max_time_seconds": a.max_time_seconds,
                   "seed": a.seed, "lexicographic_objective": a.objective == "vehicles-then-distance",
                   "hard_max_duration": a.hard_max_duration,
                   "hard_capacity": a.hard_capacity,
                   "hard_time_windows": a.hard_time_windows},
        "dimension_count": dim,
        "demand_sign_convention": a.demand_sign,   # 1 = delivery demand positive
        "locations": locations,
        "depots": depots,
        "vehicles": vehicles,
        "tasks": tasks,
        "requests": requests,
        "travel": {"location_count": N, "distances": dflat, "durations": uflat},
    }
    return req, n_own, n_sub, collapsed


if __name__ == "__main__":
    main()
