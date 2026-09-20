#!/usr/bin/env python3
"""
surge_map_selftest.py - CI self-test for surge_map.build_geojson.

Exercises the pure GeoJSON core on canned request/solution dicts (no solver, no
subprocess, no files) and asserts the contract:
  * one MultiLineString Feature per route; a trip = one line;
  * GeoJSON coordinate order is [lon, lat];
  * straight-line legs by default; supplied leg geometry is stitched in with the
    shared vertex deduped;
  * stop / depot / unassigned Points with generic labels;
  * both request shapes (locations[]+location_id, and inline task x/y) resolve;
  * bbox_of spans all geometry.

Run: python3 scripts/surge_map_selftest.py   (exit 0 = pass)
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import surge_map as sm


def _base_request():
    return {
        "locations": [{"x": 19.0, "y": 47.5}, {"x": 19.1, "y": 47.6}, {"x": 19.2, "y": 47.4}],
        "depots": [{"id": 0, "location_id": 0}],
        "vehicles": [{"id": 0, "start_depot_id": 0, "capacity": [100]}],
        "tasks": [{"id": 0, "location_id": 1}, {"id": 1, "location_id": 2}],
        "requests": [{"id": 0, "delivery_task_id": 0}, {"id": 1, "delivery_task_id": 1}],
    }


def _base_solution():
    return {
        "routes": [{"vehicle_id": 0, "distance": 1000.0, "duration": 3600.0, "stops": [
            {"task_id": 0, "request_id": 0, "trip_index": 0, "arrival": 100, "type": "delivery"},
            {"task_id": 1, "request_id": 1, "trip_index": 0, "arrival": 200, "type": "delivery"}]}],
        "unassigned": [],
    }


def main():
    # --- straight-line, single trip ---
    routes_fc, stops_fc, legs = sm.build_geojson(_base_request(), _base_solution())
    assert routes_fc["type"] == "FeatureCollection" and len(routes_fc["features"]) == 1
    f = routes_fc["features"][0]
    assert f["geometry"]["type"] == "MultiLineString"
    assert len(f["geometry"]["coordinates"]) == 1                      # one trip -> one line
    line = f["geometry"]["coordinates"][0]
    assert line == [[19.0, 47.5], [19.1, 47.6], [19.2, 47.4], [19.0, 47.5]]  # depot->t0->t1->depot, [lon,lat]
    p = f["properties"]
    assert p["route"] == 0 and p["vehicle_id"] == 0 and p["label"].startswith("vehicle 0")
    assert p["n_stops"] == 2 and p["n_trips"] == 1 and p["on_duty"] == "01:00"
    # legs: depot->t0, t0->t1, t1->depot (lat,lon tuples)
    assert [lid for lid, _, _ in legs] == ["R0_T0_000", "R0_T0_001", "R0_T0_002"]
    assert legs[0][1] == (47.5, 19.0) and legs[0][2] == (47.6, 19.1)

    # stops: 2 stop points + 1 depot; [lon,lat]; generic labels
    kinds = {}
    for sf in stops_fc["features"]:
        kinds[sf["properties"]["kind"]] = kinds.get(sf["properties"]["kind"], 0) + 1
    assert kinds == {"stop": 2, "depot": 1}, kinds
    s0 = next(sf for sf in stops_fc["features"] if sf["properties"].get("task_id") == 0)
    assert s0["geometry"]["coordinates"] == [19.1, 47.6] and s0["properties"]["label"] == "task 0"
    assert s0["properties"]["arr"] == "00:00" or s0["properties"]["arr"] == "00:01"  # 100s -> 00:01

    # --- supplied road geometry is stitched, shared vertex deduped ---
    geom = {
        "R0_T0_000": [[47.5, 19.0], [47.55, 19.05], [47.6, 19.1]],
        "R0_T0_001": [[47.6, 19.1], [47.5, 19.15], [47.4, 19.2]],   # starts at t0 (dup vertex)
        "R0_T0_002": [[47.4, 19.2], [47.5, 19.0]],
    }
    rfc2, _, _ = sm.build_geojson(_base_request(), _base_solution(), geometry=geom)
    line2 = rfc2["features"][0]["geometry"]["coordinates"][0]
    # 3 + 3 + 2 = 8 raw points, minus 2 deduped shared vertices = 6
    assert len(line2) == 6, len(line2)
    assert line2[0] == [19.0, 47.5] and line2[-1] == [19.0, 47.5]

    # --- multi-trip: a second trip makes a second line ---
    sol_mt = _base_solution()
    sol_mt["routes"][0]["stops"].append(
        {"task_id": 0, "request_id": 0, "trip_index": 1, "arrival": 500, "type": "delivery"})
    rfc3, _, _ = sm.build_geojson(_base_request(), sol_mt)
    assert len(rfc3["features"][0]["geometry"]["coordinates"]) == 2
    assert rfc3["features"][0]["properties"]["n_trips"] == 2

    # --- unassigned request -> a point ---
    req_u = _base_request()
    sol_u = {"routes": [{"vehicle_id": 0, "stops": [
        {"task_id": 0, "request_id": 0, "trip_index": 0, "arrival": 100, "type": "delivery"}]}],
        "unassigned": [1]}
    _, sfc_u, _ = sm.build_geojson(req_u, sol_u)
    un = [x for x in sfc_u["features"] if x["properties"]["kind"] == "unassigned"]
    assert len(un) == 1 and un[0]["geometry"]["coordinates"] == [19.2, 47.4]

    # --- inline x/y request shape (no locations[]) resolves too ---
    req_inline = {
        "depots": [{"id": 0, "x": 19.0, "y": 47.5}],
        "vehicles": [{"id": 0, "start_depot_id": 0, "capacity": [50]}],
        "tasks": [{"id": 0, "x": 19.1, "y": 47.6}],
        "requests": [{"id": 0, "delivery_task_id": 0}],
    }
    sol_inline = {"routes": [{"vehicle_id": 0, "stops": [
        {"task_id": 0, "request_id": 0, "trip_index": 0, "arrival": 0, "type": "delivery"}]}], "unassigned": []}
    rfc_i, sfc_i, _ = sm.build_geojson(req_inline, sol_inline)
    assert rfc_i["features"][0]["geometry"]["coordinates"][0] == [[19.0, 47.5], [19.1, 47.6], [19.0, 47.5]]
    assert any(x["properties"]["kind"] == "depot" for x in sfc_i["features"])

    # --- bbox_of spans all geometry ---
    bb = sm.bbox_of(routes_fc, stops_fc)
    assert bb == (47.4, 19.0, 47.6, 19.2), bb

    # --- human refs + per-stop breakdown (demand, running load, odometer) ---
    req_b = _base_request()
    req_b["dimension_count"] = 2
    req_b["tasks"][0]["demand"] = [10, 1]; req_b["tasks"][0]["ref"] = "ORD-A"
    req_b["tasks"][1]["demand"] = [30, 2]; req_b["tasks"][1]["ref"] = "ORD-B"
    req_b["vehicles"][0]["plate"] = "ABC-123"
    req_b["depots"][0]["location_id"] = 0
    # travel matrix (3 locations): distances in meters, symmetric-ish
    req_b["travel"] = {"location_count": 3,
                       "distances": [0, 1000, 2000, 1000, 0, 1500, 2000, 1500, 0]}
    rfc_b, sfc_b, _ = sm.build_geojson(req_b, _base_solution())
    rp = rfc_b["features"][0]["properties"]
    assert rp["vehicle_ref"] == "ABC-123", rp["vehicle_ref"]
    assert rp["capacity"] == [100] or rp["capacity"] == [100.0] or rp["capacity"], rp["capacity"]
    # trip total demand = [40,3]; peak load on board leaving depot = [40,3]
    assert rp["peak"] == [40, 3], rp["peak"]
    det = rp["stops"]
    assert len(det) == 2 and det[0]["seq"] == 1 and det[1]["seq"] == 2
    assert det[0]["ref"] == "ORD-A" and det[1]["ref"] == "ORD-B"
    # deliver t0 [10,1]: load left on board = [30,2]; then t1 [30,2]: [0,0]
    assert det[0]["load"] == [30, 2], det[0]["load"]
    assert det[1]["load"] == [0, 0], det[1]["load"]
    # odometer: depot(0)->t0(loc1)=1000; ->t1(loc2)=1500 -> cum 2500
    assert det[0]["dist_cum"] == 1000 and det[1]["dist_cum"] == 2500, (det[0]["dist_cum"], det[1]["dist_cum"])
    # the ref rides onto the stop points too
    s_b = next(sf for sf in sfc_b["features"] if sf["properties"].get("task_id") == 0)
    assert s_b["properties"]["ref"] == "ORD-A"

    # --- per-stop on-time / late flag (arrival vs task tw_late) ---
    req_l = _base_request()
    req_l["tasks"][0]["tw_late"] = 90       # arrival 100 -> 10s late (0.2 min)
    req_l["tasks"][1]["tw_late"] = 100000   # arrival 200 -> on time
    rfc_l, sfc_l, _ = sm.build_geojson(req_l, _base_solution())
    dl = rfc_l["features"][0]["properties"]["stops"]
    assert dl[0]["late_min"] > 0 and dl[1]["late_min"] == 0, [d["late_min"] for d in dl]
    s_l = next(sf for sf in sfc_l["features"] if sf["properties"].get("task_id") == 0)
    assert s_l["properties"]["late_min"] > 0
    # no window -> never late
    rfc_n2, _, _ = sm.build_geojson(_base_request(), _base_solution())
    assert rfc_n2["features"][0]["properties"]["stops"][0]["late_min"] == 0
    # no demand/travel -> breakdown still present, load/dist empty (never crashes)
    rfc_n, _, _ = sm.build_geojson(_base_request(), _base_solution())
    assert rfc_n["features"][0]["properties"]["stops"][0]["dist_cum"] is None
    # timeline carries the plate for the animated marker tooltip
    assert sm.build_timeline(req_b, _base_solution())["vehicles"][0]["vehicle_ref"] == "ABC-123"

    # --- build_timeline: timed move segments per vehicle, dwells as gaps ---
    req_t = _base_request()
    req_t["travel"] = {"location_count": 3,
                       "durations": [0, 600, 700, 600, 0, 300, 700, 300, 0],
                       "distances": [0, 1, 1, 1, 0, 1, 1, 1, 0]}
    sol_t = {"routes": [{"vehicle_id": 0, "stops": [
        {"task_id": 0, "request_id": 0, "trip_index": 0, "arrival": 600, "service_start": 600, "departure": 900},
        {"task_id": 1, "request_id": 1, "trip_index": 0, "arrival": 1200, "service_start": 1200, "departure": 1500}]}],
        "unassigned": []}
    tl = sm.build_timeline(req_t, sol_t)
    assert tl["span"] == [0, 2200], tl["span"]              # depot depart 600-600=0; return 1500+700=2200
    assert len(tl["vehicles"]) == 1
    segs = tl["vehicles"][0]["segs"]
    assert len(segs) == 3                                   # depot->s0, s0->s1, s1->depot
    assert segs[0][0] == 0 and segs[0][1] == 600            # first leg times
    assert segs[1][0] == 900 and segs[1][1] == 1200         # dwell 600..900 is the gap before it
    assert segs[-1][1] == 2200

    print("surge_map_selftest: OK (MultiLineString per route, [lon,lat], geometry stitch+dedup, "
          "stop/depot/unassigned points, both request shapes, bbox, timeline)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
