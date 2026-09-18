#!/usr/bin/env python3
"""
matrix_selftest.py - CI self-test for build_matrix.py.

Proves the matrix assembly + indexing contract WITHOUT a graph (graphs are not
committed): it drives build_matrix's pure functions on canned route output that
stands in for velo matrix_build. Checks:
  * strict 1:1 bijection (unique keys, contiguous index, N*N matrices, zero
    diagonal);
  * the depot lands at index 0;
  * a split order (same id, two lines) gets distinct #1/#2 keys and distinct
    indices -- NO location de-duplication;
  * order_no is deliberately NOT used as the key (it is not unique here);
  * a negative control: a duplicate key must fail validation.

Run: python3 scripts/matrix_selftest.py   (exit 0 = pass)
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_matrix as bm


def canned_route(N):
    """Deterministic stand-in for matrix_build: symmetric, triangle-safe."""
    snap_node = [1000 + i for i in range(N)]
    snap_off = [float(i) for i in range(N)]
    dur = [[0.0] * N for _ in range(N)]
    dist = [[0.0] * N for _ in range(N)]
    for i in range(N):
        for j in range(N):
            if i != j:
                dur[i][j] = float(abs(i - j) * 60)      # 1 min per index step
                dist[i][j] = float(abs(i - j) * 1000)   # 1 km per index step
    return snap_node, snap_off, dur, dist


def main():
    # two stops share an id (a split order): must get #1/#2, stay distinct
    stops = [
        {"id": "o-1", "order_no": "100", "ship_addr_id": "A", "lat": 47.1, "lon": 19.1, "pallets": 5},
        {"id": "o-2", "order_no": "200", "ship_addr_id": "B", "lat": 47.2, "lon": 19.2, "pallets": 7},
        {"id": "o-3", "order_no": "200", "ship_addr_id": "B", "lat": 47.2, "lon": 19.2, "pallets": 3},  # same order_no as above!
    ]
    keys = bm.unique_keys(stops, "id")
    assert keys == ["o-1", "o-2", "o-3"], keys  # ids unique here -> no suffix

    # now force an id collision (real-world split order) and check #-suffixing
    stops_dup = stops + [{"id": "o-3", "order_no": "200", "ship_addr_id": "B",
                          "lat": 47.2, "lon": 19.2, "pallets": 9}]
    kd = bm.unique_keys(stops_dup, "id")
    assert kd[2] == "o-3#1" and kd[3] == "o-3#2", kd
    assert len(set(kd)) == len(kd), "keys not unique after suffixing"

    depot = (47.6039649, 18.6441461)
    depot_key = "DEPOT-TEST"
    N = 1 + len(stops_dup)
    snap_node, snap_off, dur, dist = canned_route(N)
    attrs = ["id", "order_no", "ship_addr_id", "lat", "lon", "pallets"]
    locs = bm.build_locations(stops_dup, kd, snap_node, snap_off, depot, depot_key, attrs, "id")
    doc = bm.assemble("truck", "test.vlg", locs, dur, dist, 0, "2020-01-01T00:00:00Z")

    # contract checks
    bm.validate_bijection(doc)
    assert doc["locations"][0]["role"] == "depot" and doc["locations"][0]["key"] == depot_key
    assert doc["meta"]["depot_index"] == 0
    assert doc["meta"]["reachability"] == "ALL PAIRS ROUTABLE"
    # split order: two distinct indices, distinct keys, same coord
    idx = {l["key"]: l["index"] for l in doc["locations"]}
    assert idx["o-3#1"] != idx["o-3#2"]
    a, b = idx["o-3#1"], idx["o-3#2"]
    assert (doc["locations"][a]["lat"], doc["locations"][a]["lon"]) == \
           (doc["locations"][b]["lat"], doc["locations"][b]["lon"])
    # order_no is NOT unique -> must not be the join key
    onos = [l.get("order_no") for l in doc["locations"] if l["role"] == "stop"]
    assert len(set(onos)) < len(onos), "test fixture should have a duplicate order_no"

    # negative control: a broken (non-bijective) doc must be rejected
    bad = {"travel": {"location_count": 2,
                      "distances": [0.0, 1.0, 1.0, 0.0],
                      "durations": [0.0, 1.0, 1.0, 0.0]},
           "locations": [{"index": 0, "key": "dup"}, {"index": 1, "key": "dup"}]}
    try:
        bm.validate_bijection(bad)
    except AssertionError:
        pass
    else:
        print("FAIL: duplicate-key doc passed bijection validation", file=sys.stderr)
        return 1

    print(f"matrix_selftest: OK (N={N}, strict bijection, split-order keys distinct, "
          "negative control rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
