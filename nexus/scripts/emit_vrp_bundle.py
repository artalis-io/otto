#!/usr/bin/env python3
"""
emit_vrp_bundle.py - Nexus pipeline step: Velo matrix + vehicles -> VRP CSV bundle.

Emits a vendor-neutral CSV deliverable set that any VRP solver (or a downstream
Surge request builder) can consume, from artifacts the pipeline already produces:

  orders.csv     one row per location (depot + order-lines), keyed by locations[].key
  distance.csv   square N x N matrix, meters   (first row/col = keys)
  duration.csv   square N x N matrix, seconds  (first row/col = keys)
  vehicles.csv   the ingested vehicle records, flattened to CSV

The distance and duration matrices come from ONE Surge matrix JSON, so they are a
consistent (distance, duration) pair for that objective (fastest or shortest --
see the matrix's meta.weight_optimized). Run once per objective you want to ship.

Dataset-agnostic: order columns are configurable and the vehicle CSV is a
faithful flatten (no fixed vehicle schema is assumed -- map columns to VRP roles
when you build the solve request).

Usage:
  emit_vrp_bundle.py --matrix velo_matrix.truck.duration.json \\
                     [--vehicles vehicles.json] [--order-cols key,role,...] \\
                     --out-dir bundle/
"""
import argparse, csv, json, os, sys

DEFAULT_ORDER_COLS = ["key", "role", "order_no", "city", "lat", "lon",
                      "pallets", "weight_kg", "tw_start", "tw_end", "service_min"]


def order_rows(locations, cols):
    """Header + one row per location, pulling `cols` from each (missing -> '')."""
    rows = [list(cols)]
    for l in locations:
        rows.append(["" if l.get(c) is None else l.get(c) for c in cols])
    return rows


def square_matrix_rows(keys, flat, N, corner="from\\to"):
    """Header + N rows: a labelled square matrix from a row-major flat list."""
    assert len(keys) == N and len(flat) == N * N, "matrix/key size mismatch"
    rows = [[corner] + list(keys)]
    for i in range(N):
        rows.append([keys[i]] + [flat[i * N + j] for j in range(N)])
    return rows


def vehicle_rows(vehicles):
    """Header (stable union of keys) + one row per vehicle record."""
    if not vehicles:
        return [["id"]]
    cols = list(vehicles[0].keys())
    seen = set(cols)
    for v in vehicles:            # append any keys not in the first record
        for k in v:
            if k not in seen:
                cols.append(k); seen.add(k)
    rows = [cols]
    for v in vehicles:
        rows.append(["" if v.get(c) is None else v.get(c) for c in cols])
    return rows


def write_csv(path, rows):
    with open(path, "w", newline="") as fh:
        csv.writer(fh).writerows(rows)


def main():
    ap = argparse.ArgumentParser(description="Emit a VRP CSV bundle from a Velo matrix + vehicles")
    ap.add_argument("--matrix", required=True, help="a velo_matrix.*.json (Surge travel object + locations)")
    ap.add_argument("--vehicles", default=None, help="ingested vehicles JSON (optional)")
    ap.add_argument("--order-cols", default=",".join(DEFAULT_ORDER_COLS))
    ap.add_argument("--out-dir", required=True)
    a = ap.parse_args()

    m = json.load(open(a.matrix))
    N = m["travel"]["location_count"]
    locs = m["locations"]
    keys = [l["key"] for l in locs]
    cols = [c for c in a.order_cols.split(",") if c]
    os.makedirs(a.out_dir, exist_ok=True)

    write_csv(os.path.join(a.out_dir, "orders.csv"), order_rows(locs, cols))
    write_csv(os.path.join(a.out_dir, "distance.csv"),
              square_matrix_rows(keys, m["travel"]["distances"], N))
    write_csv(os.path.join(a.out_dir, "duration.csv"),
              square_matrix_rows(keys, m["travel"]["durations"], N))

    nveh = 0
    if a.vehicles:
        vdoc = json.load(open(a.vehicles))
        vehicles = vdoc["records"] if isinstance(vdoc, dict) and "records" in vdoc else vdoc
        write_csv(os.path.join(a.out_dir, "vehicles.csv"), vehicle_rows(vehicles))
        nveh = len(vehicles)

    weight = m["meta"].get("weight_optimized", "?")
    print(f"emit_vrp_bundle: N={N} locations, {nveh} vehicles, objective={weight.split(' ')[0]} "
          f"-> {a.out_dir}/{{orders,distance,duration{',vehicles' if nveh else ''}}}.csv",
          file=sys.stderr)


if __name__ == "__main__":
    main()
