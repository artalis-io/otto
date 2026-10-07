import { test } from 'node:test';
import assert from 'node:assert/strict';

import { applyOverrides } from './solve.js';
import type { SurgeRequest } from './types.js';
import type { Scenario } from './store.js';

/* Minimal request: two orders (O1->task 10->req 100, O2->task 11->req 101),
 * three vehicles (1,2,3). Only the override-relevant fields are populated. */
function mkRequest(): SurgeRequest {
  return {
    config: {}, dimension_count: 2, demand_sign_convention: 1,
    locations: [], depots: [],
    vehicles: [
      { id: 1, ref: 'V1', capacity: [1, 1], start_depot_id: 0, end_depot_id: 0 },
      { id: 2, ref: 'V2', capacity: [1, 1], start_depot_id: 0, end_depot_id: 0 },
      { id: 3, ref: 'V3', capacity: [1, 1], start_depot_id: 0, end_depot_id: 0 },
    ],
    tasks: [
      { id: 10, type: 'delivery', location_id: 1, tw_early: 0, tw_late: 0, service_seconds: 0, demand: [1, 1], ref: 'O1' },
      { id: 11, type: 'delivery', location_id: 2, tw_early: 0, tw_late: 0, service_seconds: 0, demand: [1, 1], ref: 'O2' },
    ],
    requests: [
      { id: 100, delivery_task_id: 10 },
      { id: 101, delivery_task_id: 11 },
    ],
    travel: { location_count: 0, distances: [], durations: [] },
  };
}

function mkScenario(over: Partial<Scenario> = {}): Scenario {
  return {
    id: 'scn', day: 'day1', kind: 'copy', parentId: 'day1-base', revision: 1,
    label: 'edited', removedVehicleIds: [], pins: [], forbids: [], createdAt: '',
    ...over,
  };
}

test('applyOverrides: pin sets allowed_vehicles to the single pinned vehicle', () => {
  const req = mkRequest();
  applyOverrides(req, mkScenario({ pins: [{ orderNo: 'O1', vehicleId: 2 }] }));
  assert.deepEqual(req.requests[0]!.allowed_vehicles, [2]);
  assert.equal(req.requests[1]!.allowed_vehicles, undefined);
});

test('applyOverrides: forbid appends to forbidden_vehicles', () => {
  const req = mkRequest();
  applyOverrides(req, mkScenario({ forbids: [{ orderNo: 'O2', vehicleId: 1 }, { orderNo: 'O2', vehicleId: 3 }] }));
  assert.deepEqual(req.requests[1]!.forbidden_vehicles, [1, 3]);
  assert.equal(req.requests[1]!.allowed_vehicles, undefined);
});

test('applyOverrides: pin on an order supersedes a forbid on the same order', () => {
  const req = mkRequest();
  applyOverrides(req, mkScenario({
    pins: [{ orderNo: 'O1', vehicleId: 2 }],
    forbids: [{ orderNo: 'O1', vehicleId: 3 }],
  }));
  assert.deepEqual(req.requests[0]!.allowed_vehicles, [2]);
  assert.equal(req.requests[0]!.forbidden_vehicles, undefined);
});

test('applyOverrides: overrides naming an unavailable vehicle are skipped', () => {
  const req = mkRequest();
  const avail = new Set([1, 3]); // vehicle 2 removed from the fleet
  applyOverrides(req, mkScenario({
    pins: [{ orderNo: 'O1', vehicleId: 2 }],
    forbids: [{ orderNo: 'O2', vehicleId: 2 }],
  }), avail);
  assert.equal(req.requests[0]!.allowed_vehicles, undefined);
  assert.equal(req.requests[1]!.forbidden_vehicles, undefined);
});
