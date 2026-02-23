#!/usr/bin/env python3
"""Basic VRP example — 2 vehicles, 3 deliveries.

Run from surge/bindings/python/:
    SURGE_LIB_PATH=../../libsurge_api.dylib python examples/basic_vrp.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import surge  # noqa: E402

problem = {
    "config": {
        "max_iterations": 500,
        "seed": 42,
        "deterministic": True,
    },
    "depots": [
        {"x": 0, "y": 0, "tw_early": 0, "tw_late": 86400},
    ],
    "vehicles": [
        {
            "start_depot_id": 0,
            "end_depot_id": 0,
            "shift_early": 0,
            "shift_late": 86400,
            "capacity": [100],
        },
        {
            "start_depot_id": 0,
            "end_depot_id": 0,
            "shift_early": 0,
            "shift_late": 86400,
            "capacity": [100],
        },
    ],
    "tasks": [
        {
            "type": "delivery",
            "x": 10,
            "y": 10,
            "tw_early": 0,
            "tw_late": 86400,
            "service_seconds": 300,
            "demand": [-20],
        },
        {
            "type": "delivery",
            "x": -15,
            "y": 5,
            "tw_early": 0,
            "tw_late": 86400,
            "service_seconds": 300,
            "demand": [-30],
        },
        {
            "type": "delivery",
            "x": 20,
            "y": -10,
            "tw_early": 0,
            "tw_late": 86400,
            "service_seconds": 300,
            "demand": [-25],
        },
    ],
    "requests": [
        {"delivery_task_id": 0},
        {"delivery_task_id": 1},
        {"delivery_task_id": 2},
    ],
}

print(f"Surge version: {surge.version()}")
print(f"Health: {surge.health()}")
print()

result = surge.solve(problem)

print(f"Status: {result['status']}")
print(f"Vehicles used: {result['stats']['vehicles_used']}")
print(f"Total distance: {result['stats']['total_distance']:.2f}")
print(f"Unassigned: {result['stats']['unassigned']}")
print()

for i, route in enumerate(result["routes"]):
    print(f"Route {i} (vehicle {route['vehicle_id']}):")
    print(f"  Distance: {route['distance']:.2f}")
    print(f"  Duration: {route['duration']:.2f}")
    for stop in route["stops"]:
        print(
            f"  Stop: request={stop['request_id']} "
            f"type={stop['type']} "
            f"arrive={stop['arrival']:.1f} "
            f"depart={stop['departure']:.1f}"
        )
    print()
