#!/usr/bin/env python3
"""
surge_request_selftest.py - CI self-test for build_surge_request.py.

Exercises assemble_request() on canned bundle rows (no solver, no files) and
asserts the request contract:
  * depot at index 0 (location_id 0, shift window);
  * each delivery task's location_id == its travel-matrix index;
  * travel block: location_count + N*N flattened matrices;
  * 2-D demand from demand-cols; demand_sign_convention = 1 (delivery positive);
  * vehicles: own + subcontractor clones (high fixed cost), multi-trip fields;
  * config carries max_iterations (required) + lexicographic per objective;
  * requests link 1:1 to tasks.

Run: python3 scripts/surge_request_selftest.py   (exit 0 = pass)
"""
import os, sys
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_surge_request as bsr


def main():
    ocols = ["key", "role", "order_no", "city", "lat", "lon",
             "pallets", "weight_kg", "tw_start", "tw_end", "service_min"]
    orows = [
        ["DEPOT", "depot", "", "", "47.6", "18.6", "", "", "", "", ""],
        ["o-1", "stop", "100", "A", "47.1", "19.1", "5", "1000", "08:00", "16:00", "30"],
        ["o-2", "stop", "200", "B", "47.2", "19.2", "7", "2000", "", "", "60"],
    ]
    oidx = {c: ocols.index(c) for c in ocols}
    N = 3
    dflat = [0, 1000, 2000, 1000, 0, 1500, 2000, 1500, 0]
    uflat = [0, 100, 200, 100, 0, 150, 200, 150, 0]
    vrows = [
        ["id", "plate", "capacity_pallets", "capacity_kg", "is_subcontractor"],
        ["v-own", "AAA-1", "33", "24000", "false"],
        ["v-sub", "BBB-2", "17", "12000", "true"],
    ]
    a = SimpleNamespace(
        shift="06:00-18:00", max_duration_min=540,
        demand_cols="pallets,weight_kg", capacity_cols="capacity_pallets,capacity_kg",
        sub_col="is_subcontractor", sub_clones=5,
        own_fixed_cost=100.0, sub_fixed_cost=100000.0,
        max_trips=2, trip_reload_seconds=1800,
        objective="vehicles-then-distance",
        max_iterations=20000, max_time_seconds=30, seed=42,
        demand_sign=1, unassigned_penalty=1e6, hard_max_duration=True, hard_capacity=True,
        plate_col="plate", dedupe_vehicle_configs=True,
    )
    req, n_own, n_sub, collapsed = bsr.assemble_request(orows, oidx, dflat, uflat, N, vrows, a)

    # depot at index 0
    assert len(req["depots"]) == 1
    assert req["depots"][0]["location_id"] == 0
    assert req["depots"][0]["tw_early"] == 21600 and req["depots"][0]["tw_late"] == 64800

    # tasks: location_id == travel index (1,2), delivery, 2-D demand, tw parse
    assert len(req["tasks"]) == 2 and len(req["requests"]) == 2
    t0, t1 = req["tasks"]
    assert t0["location_id"] == 1 and t1["location_id"] == 2
    assert t0["type"] == "delivery"
    assert t0["demand"] == [5, 1000] and t1["demand"] == [7, 2000]
    assert t0["tw_early"] == 8 * 3600 and t0["tw_late"] == 16 * 3600      # from 08:00-16:00
    assert t1["tw_early"] == 21600 and t1["tw_late"] == 64800             # open -> shift
    assert t0["service_seconds"] == 1800 and t1["service_seconds"] == 3600
    assert [r["delivery_task_id"] for r in req["requests"]] == [0, 1]

    # travel block
    assert req["travel"]["location_count"] == N
    assert len(req["travel"]["distances"]) == N * N == len(req["travel"]["durations"])

    # dimensions + sign
    assert req["dimension_count"] == 2
    assert req["demand_sign_convention"] == 1

    # vehicles: 1 own + 5 sub-clones, capacity dims, multi-trip fields, cost split
    assert n_own == 1 and n_sub == 5 and len(req["vehicles"]) == 6
    own = req["vehicles"][0]
    assert own["capacity"] == [33, 24000] and own["start_depot_id"] == 0 and own["end_depot_id"] == 0
    assert own["max_trips"] == 2 and own["trip_reload_seconds"] == 1800
    assert own["fixed_cost"] == 100.0
    subs = req["vehicles"][1:]
    assert all(v["fixed_cost"] == 100000.0 and v["capacity"] == [17, 12000] for v in subs)
    assert collapsed == []   # base fixture has no shared base plates

    # vehicle-config dedup: a drawbar truck listed twice (solo rigid + rigid+trailer
    # combo) shares a base plate; only the higher-capacity combo must survive.
    vrows2 = [
        ["id", "plate", "capacity_pallets", "capacity_kg", "is_subcontractor"],
        ["r-solo",  "SLZ-098",         "17", "12000", "false"],   # rigid solo
        ["r-combo", "SLZ-098+TRL-1",   "34", "24000", "false"],   # same truck + drawbar
        ["r-plain", "OTH-2",           "17", "14000", "false"],   # unrelated own truck
        ["v-sub",   "BBB-2",           "33", "24000", "true"],
    ]
    req2, n_own2, n_sub2, collapsed2 = bsr.assemble_request(orows, oidx, dflat, uflat, N, vrows2, a)
    assert n_own2 == 2, f"expected 2 physical own trucks after dedup, got {n_own2}"   # combo + OTH-2
    assert len(collapsed2) == 1 and collapsed2[0][0] == "SLZ-098"
    own_caps = sorted(v["capacity"][1] for v in req2["vehicles"][:2])
    assert own_caps == [14000, 24000], own_caps        # solo 12000 dropped, combo 24000 kept
    # dedup can be turned off
    a_off = SimpleNamespace(**{**a.__dict__, "dedupe_vehicle_configs": False})
    _, n_own_off, _, collapsed_off = bsr.assemble_request(orows, oidx, dflat, uflat, N, vrows2, a_off)
    assert n_own_off == 3 and collapsed_off == []

    # config
    assert req["config"]["max_iterations"] == 20000
    assert req["config"]["lexicographic_objective"] is True

    print("surge_request_selftest: OK (depot@0, task<->travel-index, 2D demand, "
          "delivery sign, own+sub fleet, multi-trip fields, config)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
