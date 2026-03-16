"""Tests for the Surge Python binding."""

import pytest

import surge


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def single_delivery_problem():
    """1 vehicle, 1 delivery — minimal solvable problem."""
    return {
        "config": {"max_iterations": 100, "seed": 42, "deterministic": True},
        "depots": [{"x": 0, "y": 0, "tw_early": 0, "tw_late": 10000}],
        "vehicles": [
            {
                "start_depot_id": 0,
                "end_depot_id": 0,
                "shift_early": 0,
                "shift_late": 10000,
                "capacity": [100],
            }
        ],
        "tasks": [
            {
                "type": "delivery",
                "x": 10,
                "y": 0,
                "tw_early": 0,
                "tw_late": 10000,
                "service_seconds": 60,
                "demand": [-10],
            }
        ],
        "requests": [{"delivery_task_id": 0}],
    }


def pickup_delivery_problem():
    """1 vehicle, 1 PD pair."""
    return {
        "config": {"max_iterations": 100, "seed": 42, "deterministic": True},
        "depots": [{"x": 0, "y": 0, "tw_early": 0, "tw_late": 10000}],
        "vehicles": [
            {
                "start_depot_id": 0,
                "end_depot_id": 0,
                "shift_early": 0,
                "shift_late": 10000,
                "capacity": [100],
            }
        ],
        "tasks": [
            {
                "type": "pickup",
                "x": 5,
                "y": 0,
                "tw_early": 0,
                "tw_late": 10000,
                "service_seconds": 30,
                "demand": [10],
            },
            {
                "type": "delivery",
                "x": 15,
                "y": 0,
                "tw_early": 0,
                "tw_late": 10000,
                "service_seconds": 30,
                "demand": [-10],
            },
        ],
        "requests": [{"pickup_task_id": 0, "delivery_task_id": 1}],
    }


def multi_vehicle_problem():
    """2 vehicles, 3 deliveries spread apart."""
    return {
        "config": {"max_iterations": 200, "seed": 42, "deterministic": True},
        "depots": [{"x": 0, "y": 0, "tw_early": 0, "tw_late": 10000}],
        "vehicles": [
            {
                "start_depot_id": 0,
                "end_depot_id": 0,
                "shift_early": 0,
                "shift_late": 10000,
                "capacity": [100],
            },
            {
                "start_depot_id": 0,
                "end_depot_id": 0,
                "shift_early": 0,
                "shift_late": 10000,
                "capacity": [100],
            },
        ],
        "tasks": [
            {
                "type": "delivery",
                "x": 10,
                "y": 10,
                "tw_early": 0,
                "tw_late": 10000,
                "service_seconds": 60,
                "demand": [-10],
            },
            {
                "type": "delivery",
                "x": -10,
                "y": -10,
                "tw_early": 0,
                "tw_late": 10000,
                "service_seconds": 60,
                "demand": [-10],
            },
            {
                "type": "delivery",
                "x": 20,
                "y": 0,
                "tw_early": 0,
                "tw_late": 10000,
                "service_seconds": 60,
                "demand": [-10],
            },
        ],
        "requests": [
            {"delivery_task_id": 0},
            {"delivery_task_id": 1},
            {"delivery_task_id": 2},
        ],
    }


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_version():
    v = surge.version()
    assert isinstance(v, str)
    assert len(v) > 0


def test_health():
    h = surge.health()
    assert h["status"] == "healthy"


def test_solve_single_delivery():
    result = surge.solve(single_delivery_problem())
    assert result["status"] in ("ok", "limit")
    assert result["stats"]["unassigned"] == 0
    assert len(result["routes"]) == 1
    assert len(result["routes"][0]["stops"]) >= 1


def test_solve_pickup_delivery():
    result = surge.solve(pickup_delivery_problem())
    assert result["status"] in ("ok", "limit")
    assert result["stats"]["unassigned"] == 0
    stops = result["routes"][0]["stops"]
    stop_types = [s["type"] for s in stops]
    # Pickup must precede delivery
    assert stop_types.index("pickup") < stop_types.index("delivery")


def test_solve_multi_vehicle():
    result = surge.solve(multi_vehicle_problem())
    assert result["status"] in ("ok", "limit")
    assert result["stats"]["unassigned"] == 0
    total_stops = sum(len(r["stops"]) for r in result["routes"])
    assert total_stops == 3


def test_empty_body_raises():
    """Invalid JSON (missing required fields like depots) should raise."""
    with pytest.raises(surge.ValidationError):
        surge.solve({"vehicles": [{"start_depot_id": 99}]})


def test_deterministic():
    problem = single_delivery_problem()
    r1 = surge.solve(problem)
    r2 = surge.solve(problem)
    assert r1["stats"]["total_distance"] == r2["stats"]["total_distance"]
    assert r1["routes"] == r2["routes"]
