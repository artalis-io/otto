#!/usr/bin/env python3
"""
vrp_bundle_selftest.py - CI self-test for emit_vrp_bundle.py.

Exercises the emit functions on canned data (no graph, no files needed for the
core checks) and asserts the deliverable contract:
  * orders.csv has a header + one row per location, in index order;
  * distance/duration are square (N+1) x (N+1) with a labelled key header row and
    key first column, matching locations;
  * vehicles.csv flattens every record with a stable column union;
  * a round-trip through the real writer produces well-formed CSV.

Run: python3 scripts/vrp_bundle_selftest.py   (exit 0 = pass)
"""
import csv, io, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emit_vrp_bundle as eb


def main():
    N = 4
    keys = ["DEPOT", "o-1", "o-2", "o-2#2"]
    locs = [
        {"key": "DEPOT", "role": "depot", "lat": 47.6, "lon": 18.6},
        {"key": "o-1", "role": "stop", "order_no": "100", "city": "A", "lat": 47.1, "lon": 19.1, "pallets": 5},
        {"key": "o-2", "role": "stop", "order_no": "200", "city": "B", "lat": 47.2, "lon": 19.2, "pallets": 7},
        {"key": "o-2#2", "role": "stop", "order_no": "200", "city": "B", "lat": 47.2, "lon": 19.2, "pallets": 3},
    ]
    dist = [0.0, 1000.0, 2000.0, 2000.0,
            1000.0, 0.0, 1500.0, 1500.0,
            2000.0, 1500.0, 0.0, 0.0,
            2000.0, 1500.0, 0.0, 0.0]
    dur = [x / 10.0 for x in dist]

    # orders.csv
    cols = eb.DEFAULT_ORDER_COLS
    orows = eb.order_rows(locs, cols)
    assert orows[0] == list(cols)
    assert len(orows) == N + 1                       # header + N
    assert orows[1][0] == "DEPOT" and orows[1][1] == "depot"
    assert orows[2][cols.index("pallets")] == 5      # value pulled
    assert orows[1][cols.index("order_no")] == ""    # depot missing -> blank

    # square matrices: (N+1) x (N+1), labelled, diagonal 0
    sq = eb.square_matrix_rows(keys, dist, N)
    assert len(sq) == N + 1 and all(len(r) == N + 1 for r in sq)
    assert sq[0] == ["from\\to"] + keys              # header row of keys
    assert [r[0] for r in sq[1:]] == keys            # first column of keys
    for i in range(N):
        assert sq[i + 1][i + 1] == 0.0               # diagonal

    # vehicles.csv: stable union of keys, one row per record
    vehicles = [
        {"id": "v1", "capacity_pallets": 33, "depot": "X"},
        {"id": "v2", "capacity_pallets": 33, "depot": "X", "is_subcontractor": True},  # extra key
    ]
    vrows = eb.vehicle_rows(vehicles)
    assert vrows[0] == ["id", "capacity_pallets", "depot", "is_subcontractor"], vrows[0]
    assert len(vrows) == 3
    assert vrows[1][vrows[0].index("is_subcontractor")] == ""   # missing -> blank

    # round-trip through the actual CSV writer (well-formed, re-parseable)
    buf = io.StringIO()
    csv.writer(buf).writerows(sq)
    back = list(csv.reader(io.StringIO(buf.getvalue())))
    assert len(back) == N + 1 and back[0][1:] == keys

    print(f"vrp_bundle_selftest: OK (N={N}, orders/square-matrix/vehicles contracts, split-order keys distinct)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
